/*
 * Native backend entry: orchestrates NYIR lowering, optimization,
 * emission, object writing, and linking into a single compilation path.
 */
#include "code/lang/pipeline/native_vm.h"
#include "base/common.h"
#include "base/time.h"
#include "base/util.h"
#include "code/backend/abi.h"
#include "code/backend/internal.h"
#include "code/backend/ir/internal.h"
#include "code/lang/llvm/jit.h"
#include "code/lang/pipeline/build.h"
#include "code/lang/runtime/aliases.h"
#include "code/lang/runtime/shared.h"
#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <nyr/ir.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

extern int64_t rt_bigint_to_str(int64_t value);
extern int64_t rt_bigint_to_cstr_raw(int64_t value);
extern int64_t rt_bigint_to_int(int64_t value);
extern int64_t rt_bigfloat_to_str(int64_t value);
extern int64_t rt_to_str(int64_t value);
extern int64_t rt_long(int64_t value);
extern int64_t rt_any_to_i64(int64_t value);
extern int64_t rt_bigint_neg_raw(int64_t value);
extern int64_t rt_sequence_len_raw(int64_t value);
extern double rt_atof(int64_t value);
extern double rt_bigfloat_to_f64_raw(int64_t value);
extern double rt_flt_unbox_double(int64_t value);
extern double rt_complex_re_f64(int64_t value);
extern double rt_complex_im_f64(int64_t value);
extern double rt_value_to_f64(int64_t value, int64_t tag);
extern double rt_vec_dot_raw(int64_t left, int64_t right);
extern double rt_any_to_f64(int64_t value);
extern double rt_sqrt_f64(double value);
extern double rt_sin_f64(double value);
extern double rt_cos_f64(double value);
extern double rt_f64_round(double value);
extern double rt_f64_pow(double base, double exponent);
extern double rt_f64_floor(double value);
extern double rt_f64_ceil(double value);
extern double rt_fmod_f64(double left, double right);
extern double rt_f64_min(double left, double right);
extern double rt_f64_max(double left, double right);
extern double rt_tbuf_index_f64(int64_t buffer, int64_t index);
extern double rt_load32_f64(int64_t addr, int64_t index);
extern int64_t rt_store32_f64(int64_t addr, int64_t index, double value);
extern int64_t rt_f64_to_i64(double value);
extern int64_t rt_f64_bits(double value);
extern int64_t rt_print_f64_raw(double value);
extern int64_t rt_f64_to_cstr_raw(double value);
extern int64_t rt_trace_arg_f64(double value);
extern int64_t rt_set_panic_env(void *env);
extern int64_t rt_clear_panic_env(void);
extern void rt_set_panic_val(int64_t value);
extern int64_t rt_get_panic_val(void);
extern int64_t rt_cstr_len(int64_t value);
extern int64_t rt_native_is_str(int64_t value);

static bool ny_native_vm_call_resolve_impl(void *opaque, const char *symbol,
                                           const int64_t *args,
                                           size_t arg_count, int64_t *out,
                                           char *err, size_t err_len);

/*
 * Non-LLVM native backend entry point and target registry.
 *
 * LLVM remains the default backend. When a native backend is explicitly
 * selected, unsupported registered targets must fail with a precise diagnostic
 * instead of silently falling back. x86-64 is the only assembly emitter today;
 * other registered target names exist so the roadmap can add emitters
 * incrementally behind stable option parsing and tests.
 */

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

static bool ny_native_write_eval_profile(const ny_options *opt,
                                         const nyr_eval_result_t *result,
                                         const char *name, char *err,
                                         size_t err_len) {
  if (!opt || !opt->nyr_run_profile)
    return true;
  FILE *out = stderr;
  if (opt->nyr_run_profile_path && opt->nyr_run_profile_path[0]) {
    nyr_ensure_parent_dir_for_path(opt->nyr_run_profile_path);
    out = fopen(opt->nyr_run_profile_path, "wb");
    if (!out) {
      nyr_set_error(err, err_len,
                        "native NYIR VM profile: failed to open %s: %s",
                        opt->nyr_run_profile_path, strerror(errno));
      return false;
    }
  }
  nyr_eval_result_dump(out, name, result);
  if (out != stderr)
    fclose(out);
  return true;
}

static bool ny_native_write_eval_result(const ny_options *opt,
                                        const nyr_eval_result_t *result,
                                        const char *name, char *err,
                                        size_t err_len) {
  FILE *out = stderr;
  if (opt && opt->nyr_run_path && opt->nyr_run_path[0]) {
    nyr_ensure_parent_dir_for_path(opt->nyr_run_path);
    out = fopen(opt->nyr_run_path, "wb");
    if (!out) {
      nyr_set_error(err, err_len, "native NYIR VM: failed to open %s: %s",
                        opt->nyr_run_path, strerror(errno));
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
  if (opt && opt->nyr_run_max_steps >= 0)
    return (size_t)opt->nyr_run_max_steps;
  return 1000000;
}

static size_t ny_native_vm_recursion_limit(const ny_options *opt) {
  if (opt && opt->nyr_run_recursion_limit >= 0)
    return (size_t)opt->nyr_run_recursion_limit;
  return 256;
}

static bool ny_native_eval_ir_func(nyr_func_t *rt_main, const ny_options *opt,
                                   const char *name, char *err,
                                   size_t err_len) {
  size_t local_count = nyr_local_count(rt_main);
  int64_t *locals = ny_native_locals_acquire(0, local_count);
  if (local_count && !locals) {
    nyr_set_error(err, err_len, NY_NATIVE_OOM);
    return false;
  }
  nyr_eval_result_t result = {0};
  bool ok = nyr_ir_eval(rt_main, locals, local_count, ny_native_vm_max_steps(opt),
                      &result, err, err_len);
  if (!ok)
    return false;
  bool wrote = ny_native_write_eval_result(opt, &result, name, err, err_len);
  nyr_eval_result_free(&result);
  return wrote;
}

typedef struct {
  const char *symbol;
  size_t index;
} ny_native_vm_symbol_cache_t;

typedef struct {
  nyr_func_t *funcs;
  const char *const *names;
  size_t count;
  size_t depth;
  size_t recursion_limit;
  size_t max_steps;
  nyr_eval_result_t *profile;
  /*
   * Borrowed symbols remain live with the immutable NYIR program for this
   * evaluation. Cache misses too: runtime calls should not repeatedly scan
   * every source function before falling through to the runtime resolver.
   */
  ny_native_vm_symbol_cache_t symbol_cache[256];
} ny_native_vm_call_ctx_t;

static void ny_native_vm_profile_take_detail(nyr_eval_result_t *dst,
                                             nyr_eval_result_t *src) {
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

static void ny_native_vm_profile_merge(nyr_eval_result_t *dst,
                                       const nyr_eval_result_t *src) {
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
  const char *canonical =
      strncmp(symbol, "ny_fn_", 6) == 0 ? symbol + 6 : symbol;
  if (strcmp(canonical, name) == 0)
    return true;
  /*
   * Imported aliases can omit one or more module prefixes.
   */
  size_t canonical_len = strlen(canonical);
  size_t name_len = strlen(name);
  if (canonical_len > name_len &&
      canonical[canonical_len - name_len - 1] == '.' &&
      strcmp(canonical + canonical_len - name_len, name) == 0)
    return true;
  return name_len > canonical_len &&
         name[name_len - canonical_len - 1] == '.' &&
         strcmp(name + name_len - canonical_len, canonical) == 0;
}

static size_t ny_native_vm_find_symbol(ny_native_vm_call_ctx_t *ctx,
                                       const char *symbol) {
  size_t slot = ny_fnv1a64_cstr(symbol, UINT64_C(1469598103934665603)) &
                (sizeof(ctx->symbol_cache) / sizeof(ctx->symbol_cache[0]) - 1);
  ny_native_vm_symbol_cache_t *cached = &ctx->symbol_cache[slot];
  if (cached->symbol && strcmp(cached->symbol, symbol) == 0)
    return cached->index;
  size_t exact_match = SIZE_MAX;
  size_t alias_match = SIZE_MAX;
  size_t alias_count = 0;
  for (size_t i = 0; i < ctx->count; ++i) {
    const char *name = ctx->names[i];
    if (!name)
      continue;
    if (strcmp(symbol, name) == 0 ||
        (strncmp(symbol, "ny_fn_", 6) == 0 && strcmp(symbol + 6, name) == 0)) {
      exact_match = i;
      break;
    }
    if (ny_native_vm_symbol_matches(symbol, name)) {
      alias_match = i;
      alias_count++;
    }
  }
  size_t index = exact_match != SIZE_MAX ? exact_match
                 : alias_count == 1      ? alias_match
                                         : SIZE_MAX;
  *cached = (ny_native_vm_symbol_cache_t){symbol, index};
  return index;
}

static bool ny_native_vm_call_resolve(void *opaque, const char *symbol,
                                      const int64_t *args, size_t arg_count,
                                      int64_t *out, char *err, size_t err_len);

/*
 * Run one popped defer entry inside the NYIR VM. Unlike ordinary dynamic
 * callbacks (whose code handle and capture environment travel packed in a
 * heap closure), lowering pushes defer fn/env as two separate words: fn is
 * either a bare NYIR symbol handle (no-capture lambdas lower to
 * NYIR_ADDR_SYMBOL) or the code word loaded out of a capture closure, and
 * env is the capture block (0 when there are no captures). The generic
 * dynamic-callback path only recovers env from heap closures, so it drops
 * the entry env on the floor and capture reads see garbage. Resolve the
 * handle against the program's functions here and install env at locals[0],
 * the same hidden-environment slot the machine route passes as the first
 * argument. Returns true when the entry ran, false (no error set) when fn
 * matches no known function so the caller can try the C path.
 */
static bool ny_native_vm_run_defer_entry(ny_native_vm_call_ctx_t *ctx,
                                         int64_t fn, int64_t env, char *err,
                                         size_t err_len) {
  if (!ctx)
    return false;
  bool marked_callable = NY_DYNAMIC_CALLABLE_IS(fn);
  int64_t decoded =
      marked_callable ? (int64_t)(uintptr_t)NY_DYNAMIC_CALLABLE_DECODE(fn) : fn;
  uintptr_t handle = (uintptr_t)decoded;
  int64_t closure_env = env;
  if (is_heap_ptr(decoded)) {
    uintptr_t base = (uintptr_t)rt_untag_v(decoded);
    int64_t header = 0;
    int64_t code = 0;
    int64_t heap_env = 0;
    if (base >= sizeof(int64_t) &&
        rt_try_read_i64(base - sizeof(int64_t), &header) &&
        header == TAG_CLOSURE && rt_try_read_i64(base, &code) &&
        base <= UINTPTR_MAX - 2 * sizeof(int64_t) &&
        rt_try_read_i64(base + sizeof(int64_t), &heap_env)) {
      /*
       * Whole-closure value pushed directly (e.g. via __push_defer).
       */
      handle = (uintptr_t)code;
      closure_env = heap_env;
    }
  }
  for (size_t i = 0; i < ctx->count; ++i) {
    const char *name = ctx->names[i];
    if (!name ||
        (handle != nyr_vm_symbol_handle(name) &&
         handle != nyr_vm_prefixed_symbol_handle("ny_fn_", name) &&
         handle != nyr_vm_prefixed_symbol_handle("__ny_lambda_", name)))
      continue;
    nyr_func_t *callee = &ctx->funcs[i];
    size_t local_count = nyr_local_count(callee);
    if (local_count < 1)
      local_count = 1;
    int64_t *locals = ny_native_locals_acquire(ctx->depth + 1, local_count);
    if (!locals)
      return nyr_set_error(err, err_len, NY_NATIVE_OOM), false;
    locals[0] = closure_env;
    nyr_eval_result_t result = {0};
    ctx->depth++;
    bool ok = nyr_ir_eval_with_calls(callee, locals, local_count, ctx->max_steps,
                                   &result, ny_native_vm_call_resolve, ctx, err,
                                   err_len);
    ctx->depth--;
    if (!ok)
      return false;
    ny_native_vm_profile_merge(ctx->profile, &result);
    if (!result.returned) {
      nyr_eval_result_free(&result);
      return nyr_set_error(err, err_len,
                               "native NYIR VM: defer '%s' did not return",
                               name),
             false;
    }
    nyr_eval_result_free(&result);
    return true;
  }
  return false;
}

static bool ny_native_vm_dynamic_callback(ny_native_vm_call_ctx_t *ctx,
                                          int64_t callable, const int64_t *args,
                                          size_t arg_count, int64_t *out,
                                          char *err, size_t err_len) {
  if (!ctx)
    return false;
  bool marked_callable = NY_DYNAMIC_CALLABLE_IS(callable);
  uint64_t callable_abi_flags = rt_dynamic_callable_abi_flags(callable);
  bool bool_result = (callable_abi_flags & NY_DYNAMIC_CALLABLE_BOOL_MARK) != 0;
  bool tagged_args =
      (callable_abi_flags & NY_DYNAMIC_CALLABLE_TAGGED_ARGS_MARK) != 0;
  bool raw_result =
      (callable_abi_flags & NY_DYNAMIC_CALLABLE_RAW_RESULT_MARK) != 0;
  int64_t decoded =
      marked_callable ? (int64_t)(uintptr_t)NY_DYNAMIC_CALLABLE_DECODE(callable)
                      : callable;
  uintptr_t handle = (uintptr_t)decoded;
  int64_t closure_env = 0;
  bool captured_closure = false;
  if (is_heap_ptr(decoded)) {
    uintptr_t base = (uintptr_t)rt_untag_v(decoded);
    int64_t header = 0;
    int64_t code = 0;
    int64_t env = 0;
    if (base >= sizeof(int64_t) &&
        rt_try_read_i64(base - sizeof(int64_t), &header) &&
        header == TAG_CLOSURE && rt_try_read_i64(base, &code) &&
        base <= UINTPTR_MAX - 2 * sizeof(int64_t) &&
        rt_try_read_i64(base + sizeof(int64_t), &env)) {
      /*
       * In the NYIR VM a closure's code word is a stable symbol handle and
       * its environment is the capture block built by native lowering. The
       * C callback trampoline understands the heap closure layout, but the
       * VM must resolve the code handle to its NYIR function and pass the
       * hidden environment parameter itself.
       */
      handle = (uintptr_t)code;
      closure_env = env;
      captured_closure = true;
    }
  }
  for (size_t i = 0; i < ctx->count; ++i) {
    const char *name = ctx->names[i];
    if (!name ||
        (handle != nyr_vm_symbol_handle(name) &&
         handle != nyr_vm_prefixed_symbol_handle("ny_fn_", name) &&
         handle != nyr_vm_prefixed_symbol_handle("__ny_lambda_", name)))
      continue;
    nyr_func_t *callee = &ctx->funcs[i];
    /*
     * NYIR function values arrive with source-level arguments. Expand them
     * into the same value/length/tag slots used by direct native calls.
     */
    size_t arg_offset = captured_closure ? 1 : 0;
    size_t source_count = callee->source_param_count;
    if (source_count && arg_count != source_count)
      return nyr_set_error(
                 err, err_len,
                 "native NYIR VM: callback '%s' argument count mismatch", name),
             false;
    size_t expanded_count = arg_offset;
    for (size_t a = 0; a < arg_count; ++a)
      expanded_count +=
          source_count && a < source_count ? callee->source_param_widths[a] : 1;
    if (expanded_count > callee->param_count)
      return nyr_set_error(
                 err, err_len,
                 "native NYIR VM: callback '%s' accepts fewer arguments", name),
             false;
    size_t local_count = nyr_local_count(callee);
    int64_t *locals = ny_native_locals_acquire(ctx->depth + 1, local_count);
    if (local_count && !locals)
      return nyr_set_error(err, err_len, NY_NATIVE_OOM), false;
    if (captured_closure)
      locals[0] = closure_env;
    size_t slot = arg_offset;
    for (size_t a = 0; a < arg_count; ++a) {
      int64_t value = args ? args[a] : 0;
      uint8_t width =
          source_count && a < source_count ? callee->source_param_widths[a] : 1;
      /*
       * `any` parameters consume tagged values even when a bool-returning
       * callable would unbox scalar arguments for a raw typed parameter.
       */
      locals[slot++] =
          rt_dynamic_callback_normalize_arg(value, width < 3 && !tagged_args);
      if (width >= 2)
        locals[slot++] = rt_sequence_len_safe(value);
      if (width >= 3)
        locals[slot++] = rt_value_tag(value);
    }
    nyr_eval_result_t result = {0};
    ctx->depth++;
    bool ok = nyr_ir_eval_with_calls(callee, locals, local_count, ctx->max_steps,
                                   &result, ny_native_vm_call_resolve, ctx, err,
                                   err_len);
    ctx->depth--;
    if (!ok)
      return false;
    ny_native_vm_profile_merge(ctx->profile, &result);
    if (!result.returned) {
      nyr_eval_result_free(&result);
      return nyr_set_error(err, err_len,
                               "native NYIR VM: callback '%s' did not return",
                               name),
             false;
    }
    if (out)
      *out = rt_dynamic_callback_normalize_result(
          result.result, bool_result,
          !marked_callable && !captured_closure && arg_count > 0 &&
              !tagged_args,
          raw_result);
    nyr_eval_result_free(&result);
    return true;
  }
  return false;
}

static bool ny_native_vm_is_closure_value(int64_t value) {
  if (!is_heap_ptr(value))
    return false;
  uintptr_t base = (uintptr_t)rt_untag_v(value);
  int64_t header = 0;
  return base >= sizeof(header) &&
         rt_try_read_i64(base - sizeof(header), &header) &&
         header == TAG_CLOSURE;
}

static bool ny_native_vm_is_function_handle(const ny_native_vm_call_ctx_t *ctx,
                                            int64_t value) {
  if (!ctx || !value)
    return false;
  uintptr_t handle = (uintptr_t)value;
  for (size_t i = 0; i < ctx->count; ++i) {
    const char *name = ctx->names[i];
    if (name &&
        (handle == nyr_vm_symbol_handle(name) ||
         handle == nyr_vm_prefixed_symbol_handle("ny_fn_", name) ||
         handle == nyr_vm_prefixed_symbol_handle("__ny_lambda_", name)))
      return true;
  }
  return false;
}

typedef enum {
  NY_NATIVE_VM_CALL_UNHANDLED,
  NY_NATIVE_VM_CALL_HANDLED,
  NY_NATIVE_VM_CALL_ERROR,
} ny_native_vm_call_status_t;

static ny_native_vm_call_status_t
ny_native_vm_call_typed_runtime(const char *symbol, const int64_t *args,
                                size_t arg_count, int64_t *out, char *err,
                                size_t err_len) {
  const ny_native_runtime_abi_decl_t *decl = ny_native_runtime_abi_decl(symbol);
  if (!decl || decl->vm_call_abi == NY_NATIVE_VM_CALL_NONE)
    return NY_NATIVE_VM_CALL_UNHANDLED;
  size_t expected =
      decl->vm_call_abi == NY_NATIVE_VM_I64_NULLARY_TO_I64 ? 0
      : decl->vm_call_abi == NY_NATIVE_VM_I64_BINARY_TO_I64 ||
              decl->vm_call_abi == NY_NATIVE_VM_F64_BINARY_TO_F64 ||
              decl->vm_call_abi == NY_NATIVE_VM_I64_BINARY_TO_F64
          ? 2
      : decl->vm_call_abi == NY_NATIVE_VM_I64_I64_F64_TO_I64    ? 3
      : decl->vm_call_abi == NY_NATIVE_VM_I64_F64_TO_I64        ? 2
      : decl->vm_call_abi == NY_NATIVE_VM_F64_I64_TO_I64        ? 2
      : decl->vm_call_abi == NY_NATIVE_VM_I64_TERNARY_TO_I64    ? 3
      : decl->vm_call_abi == NY_NATIVE_VM_I64_QUATERNARY_TO_I64 ? 4
      : decl->vm_call_abi == NY_NATIVE_VM_I64_QUINARY_TO_I64    ? 5
      : decl->vm_call_abi == NY_NATIVE_VM_I64_SENARY_TO_I64     ? 6
      : decl->vm_call_abi == NY_NATIVE_VM_I64_SEPTENARY_TO_I64  ? 7
      : decl->vm_call_abi == NY_NATIVE_VM_I64_OCTARY_TO_I64     ? 8
      : decl->vm_call_abi == NY_NATIVE_VM_I64_NONARY_TO_I64     ? 9
      : decl->vm_call_abi == NY_NATIVE_VM_I64_DECENARY_TO_I64   ? 10
      : decl->vm_call_abi == NY_NATIVE_VM_I64_UNDECENARY_TO_I64 ? 11
                                                                : 1;
  if (!out || arg_count != expected || (expected != 0 && !args)) {
    nyr_set_error(err, err_len,
                      "native NYIR VM: runtime ABI mismatch for %s: expected "
                      "%zu arguments, got %zu",
                      symbol, expected, arg_count);
    return NY_NATIVE_VM_CALL_ERROR;
  }
  switch (decl->vm_call_abi) {
  case NY_NATIVE_VM_I64_NULLARY_TO_I64:
    *out = decl->vm_call.i64_nullary_i64();
    break;
  case NY_NATIVE_VM_I64_BINARY_TO_I64:
    *out = decl->vm_call.i64_binary_i64(args[0], args[1]);
    break;
  case NY_NATIVE_VM_I64_TERNARY_TO_I64:
    *out = decl->vm_call.i64_ternary_i64(args[0], args[1], args[2]);
    break;
  case NY_NATIVE_VM_I64_QUATERNARY_TO_I64:
    *out = decl->vm_call.i64_quaternary_i64(args[0], args[1], args[2], args[3]);
    break;
  case NY_NATIVE_VM_I64_QUINARY_TO_I64:
    *out = decl->vm_call.i64_quinary_i64(args[0], args[1], args[2], args[3],
                                         args[4]);
    break;
  case NY_NATIVE_VM_I64_SENARY_TO_I64:
    *out = decl->vm_call.i64_senary_i64(args[0], args[1], args[2], args[3],
                                        args[4], args[5]);
    break;
  case NY_NATIVE_VM_I64_SEPTENARY_TO_I64:
    *out = decl->vm_call.i64_septenary_i64(args[0], args[1], args[2], args[3],
                                           args[4], args[5], args[6]);
    break;
  case NY_NATIVE_VM_I64_OCTARY_TO_I64:
    *out = decl->vm_call.i64_octary_i64(args[0], args[1], args[2], args[3],
                                        args[4], args[5], args[6], args[7]);
    break;
  case NY_NATIVE_VM_I64_NONARY_TO_I64:
    *out = decl->vm_call.i64_nonary_i64(args[0], args[1], args[2], args[3],
                                        args[4], args[5], args[6], args[7],
                                        args[8]);
    break;
  case NY_NATIVE_VM_I64_DECENARY_TO_I64:
    *out = decl->vm_call.i64_decenary_i64(args[0], args[1], args[2], args[3],
                                          args[4], args[5], args[6], args[7],
                                          args[8], args[9]);
    break;
  case NY_NATIVE_VM_I64_UNDECENARY_TO_I64:
    *out = decl->vm_call.i64_undecenary_i64(args[0], args[1], args[2], args[3],
                                            args[4], args[5], args[6], args[7],
                                            args[8], args[9], args[10]);
    break;
  case NY_NATIVE_VM_F64_UNARY_TO_I64:
    *out = decl->vm_call.f64_unary_i64(nyr_bits_to_f64(args[0]));
    break;
  case NY_NATIVE_VM_F64_UNARY_TO_F64:
    *out = nyr_f64_to_bits(
        decl->vm_call.f64_unary_f64(nyr_bits_to_f64(args[0])));
    break;
  case NY_NATIVE_VM_F64_BINARY_TO_F64:
    *out = nyr_f64_to_bits(decl->vm_call.f64_binary_f64(
        nyr_bits_to_f64(args[0]), nyr_bits_to_f64(args[1])));
    break;
  case NY_NATIVE_VM_I64_UNARY_TO_I64:
    *out = decl->vm_call.i64_unary_i64(args[0]);
    break;
  case NY_NATIVE_VM_I64_UNARY_TO_F64:
    *out = nyr_f64_to_bits(decl->vm_call.i64_unary_f64(args[0]));
    break;
  case NY_NATIVE_VM_I64_BINARY_TO_F64:
    *out = nyr_f64_to_bits(decl->vm_call.i64_binary_f64(args[0], args[1]));
    break;
  case NY_NATIVE_VM_I64_I64_F64_TO_I64:
    *out = decl->vm_call.i64_i64_f64_i64(args[0], args[1],
                                         nyr_bits_to_f64(args[2]));
    break;
  case NY_NATIVE_VM_I64_F64_TO_I64:
    *out = decl->vm_call.i64_f64_i64(args[0], nyr_bits_to_f64(args[1]));
    break;
  case NY_NATIVE_VM_F64_I64_TO_I64: {
    double value = 0.0;
    memcpy(&value, &args[0], sizeof(value));
    *out = decl->vm_call.f64_i64_i64(value, args[1]);
    break;
  }
  case NY_NATIVE_VM_CALL_NONE:
    return NY_NATIVE_VM_CALL_UNHANDLED;
  }
  return NY_NATIVE_VM_CALL_HANDLED;
}

static bool ny_native_vm_callback_abi_valid(
    const ny_native_runtime_callback_abi_decl_t *decl) {
  if (!decl || !decl->symbol || decl->source_arity > 15 ||
      !(decl->callable_repr & NY_NATIVE_ABI_VALUE_CALLBACK_MARKED) ||
      decl->callable_ownership != NY_NATIVE_ABI_BORROWED ||
      decl->environment_ownership != NY_NATIVE_ABI_BORROWED ||
      decl->result_ownership != NY_NATIVE_ABI_RESULT_ABI ||
      !(decl->result_repr & NY_NATIVE_ABI_VALUE_CALLBACK_MARKED))
    return false;
  if (decl->source_arity > 0 &&
      (!(decl->arg_repr & NY_NATIVE_ABI_VALUE_CALLBACK_MARKED) ||
       decl->arg_ownership != NY_NATIVE_ABI_BORROWED))
    return false;
  switch (decl->source_arity) {
  case 0:
    return decl->call.call0 != NULL;
  case 1:
    return decl->call.call1 != NULL;
  case 2:
    return decl->call.call2 != NULL;
  case 3:
    return decl->call.call3 != NULL;
  case 4:
    return decl->call.call4 != NULL;
  case 5:
    return decl->call.call5 != NULL;
  case 6:
    return decl->call.call6 != NULL;
  case 7:
    return decl->call.call7 != NULL;
  case 8:
    return decl->call.call8 != NULL;
  case 9:
    return decl->call.call9 != NULL;
  case 10:
    return decl->call.call10 != NULL;
  case 11:
    return decl->call.call11 != NULL;
  case 12:
    return decl->call.call12 != NULL;
  case 13:
    return decl->call.call13 != NULL;
  case 14:
    return decl->call.call14 != NULL;
  case 15:
    return decl->call.call15 != NULL;
  default:
    return false;
  }
}

static bool ny_native_vm_call_resolve_impl(void *opaque, const char *symbol,
                                           const int64_t *args,
                                           size_t arg_count, int64_t *out,
                                           char *err, size_t err_len) {
  ny_native_vm_call_ctx_t *ctx = (ny_native_vm_call_ctx_t *)opaque;
  if (!ctx || !symbol)
    return nyr_set_error(err, err_len,
                             "native NYIR VM: missing call target"),
           false;
  const char *runtime_name =
      strncmp(symbol, "ny_fn_", 6) == 0 ? symbol + 6 : symbol;
  const char *canonical_symbol = nyr_runtime_symbol(runtime_name);
  if (canonical_symbol)
    symbol = canonical_symbol;
  if (ctx->depth >= ctx->recursion_limit)
    return nyr_set_error(
               err, err_len,
               "native NYIR VM: recursive call limit exceeded at depth %zu",
               ctx->depth),
           false;
  /*
   * Function references are opaque hashes under NYIR, not readable machine
   * addresses. The native literal builder's generic tag probe classifies an
   * unknown word as a scalar; identify VM-owned function handles before that
   * tag is stored so dynamic sequence reads preserve the original handle.
   */
  if (strcmp(symbol, "rt_tag_or_raw_int") == 0 && arg_count == 1 && args &&
      ny_native_vm_is_function_handle(ctx, args[0])) {
    if (out)
      *out = TAG_CLOSURE;
    return true;
  }
  /*
   * Resolve the corresponding zero-argument indirect call in this VM.
   */
  if (strcmp(symbol, "rt_call0") == 0 && arg_count == 1 && args) {
    char callback_err[512] = {0};
    if (ny_native_vm_dynamic_callback(ctx, args[0], NULL, 0, out, callback_err,
                                      sizeof(callback_err)))
      return true;
    if (callback_err[0]) {
      nyr_set_error(err, err_len, "%s", callback_err);
      return false;
    }
  }
  /*
   * Dynamic sequence reads normally box scalar slots at the runtime
   * boundary.  A VM function symbol is an opaque hash, however, and integer
   * boxing corrupts it before the subsequent rt_mark_dynamic_* call. Keep
   * the original value only when it resolves to a function in this program.
   */
  if (strcmp(symbol, "rt_tbuf_get_any") == 0 && arg_count == 3 && args) {
    int64_t count = rt_tbuf_len_raw(args[0]);
    int64_t index = args[1];
    if (index < 0)
      index += count;
    if (index >= 0 && index < count) {
      int64_t item = rt_tbuf_get(args[0], index, 0);
      if (ny_native_vm_is_function_handle(ctx, item) ||
          ny_native_vm_is_closure_value(item)) {
        if (out)
          *out = item;
        return true;
      }
    } else if (ny_native_vm_is_function_handle(ctx, args[2]) ||
               ny_native_vm_is_closure_value(args[2])) {
      if (out)
        *out = args[2];
      return true;
    }
  }
  /*
   * Encode VM function symbols directly; the native pointer-normalization
   * helper treats opaque symbol hashes as tagged integers.
   */
  uint64_t callback_marker_flags = 0;
  bool mark_callback = true;
  if (strcmp(symbol, "rt_mark_dynamic_callable") == 0) {
    /*
     * Keep the callable marker even when the ABI uses all raw words.
     */
  } else if (strcmp(symbol, "rt_mark_dynamic_callable_raw_result") == 0) {
    callback_marker_flags = NY_DYNAMIC_CALLABLE_RAW_RESULT_MARK;
  } else if (strcmp(symbol, "rt_mark_dynamic_callable_word_args") == 0) {
    callback_marker_flags = NY_DYNAMIC_CALLABLE_WORD_ARGS_MARK;
  } else if (strcmp(symbol, "rt_mark_dynamic_callable_word_args_raw_result") ==
             0) {
    callback_marker_flags = NY_DYNAMIC_CALLABLE_WORD_ARGS_MARK |
                            NY_DYNAMIC_CALLABLE_RAW_RESULT_MARK;
  } else if (strcmp(symbol, "rt_mark_dynamic_callable_tagged_args") == 0) {
    callback_marker_flags = NY_DYNAMIC_CALLABLE_TAGGED_ARGS_MARK;
  } else if (strcmp(symbol,
                    "rt_mark_dynamic_callable_tagged_args_raw_result") == 0) {
    callback_marker_flags = NY_DYNAMIC_CALLABLE_TAGGED_ARGS_MARK |
                            NY_DYNAMIC_CALLABLE_RAW_RESULT_MARK;
  } else if (strcmp(symbol, "rt_mark_dynamic_callable_tagged_word_args") == 0) {
    callback_marker_flags = NY_DYNAMIC_CALLABLE_TAGGED_ARGS_MARK |
                            NY_DYNAMIC_CALLABLE_WORD_ARGS_MARK;
  } else if (strcmp(symbol,
                    "rt_mark_dynamic_callable_tagged_word_args_raw_result") ==
             0) {
    callback_marker_flags = NY_DYNAMIC_CALLABLE_TAGGED_ARGS_MARK |
                            NY_DYNAMIC_CALLABLE_WORD_ARGS_MARK |
                            NY_DYNAMIC_CALLABLE_RAW_RESULT_MARK;
  } else if (strcmp(symbol, "rt_mark_dynamic_bool_callable") == 0) {
    callback_marker_flags = NY_DYNAMIC_CALLABLE_BOOL_MARK;
  } else if (strcmp(symbol, "rt_mark_dynamic_bool_callable_word_args") == 0) {
    callback_marker_flags =
        NY_DYNAMIC_CALLABLE_BOOL_MARK | NY_DYNAMIC_CALLABLE_WORD_ARGS_MARK;
  } else if (strcmp(symbol, "rt_mark_dynamic_bool_callable_tagged_args") == 0) {
    callback_marker_flags =
        NY_DYNAMIC_CALLABLE_BOOL_MARK | NY_DYNAMIC_CALLABLE_TAGGED_ARGS_MARK;
  } else if (strcmp(symbol, "rt_mark_dynamic_bool_callable_tagged_word_args") ==
             0) {
    callback_marker_flags = NY_DYNAMIC_CALLABLE_BOOL_MARK |
                            NY_DYNAMIC_CALLABLE_TAGGED_ARGS_MARK |
                            NY_DYNAMIC_CALLABLE_WORD_ARGS_MARK;
  } else {
    mark_callback = false;
  }
  if (mark_callback && arg_count == 1 && args &&
      ny_native_vm_is_function_handle(ctx, args[0])) {
    if (out)
      *out = rt_dynamic_callable_encode((void *)(uintptr_t)args[0],
                                        callback_marker_flags);
    return out && *out != 0;
  }
  /*
   * Thread callback lowering passes a raw code address to the OS trampoline.
   * The interpreter's ADDR_SYMBOL values are opaque symbol handles, not
   * executable addresses. Reject this boundary before the worker can jump to
   * one; MCJIT, ORC, and native execution provide real code addresses.
   */
  if ((strcmp(symbol, "rt_thread_spawn_call_abi") == 0 ||
       strcmp(symbol, "rt_thread_launch_call_abi") == 0) &&
      arg_count == 5 && args && is_int(args[3]) && rt_untag_v(args[3]) != 0)
    return nyr_set_error(
               err, err_len,
               "native NYIR VM: thread callbacks require executable code; "
               "run this program with MCJIT, ORC, or native execution"),
           false;
  ny_native_vm_call_status_t typed_status = ny_native_vm_call_typed_runtime(
      symbol, args, arg_count, out, err, err_len);
  if (typed_status != NY_NATIVE_VM_CALL_UNHANDLED)
    return typed_status == NY_NATIVE_VM_CALL_HANDLED;
  /*
   * Defer cleanup on the NYIR VM route. Lowering registers each `defer`
   * body as a closure and emits rt_pop_run_defer/rt_run_defers_to to run
   * it. On machine routes the pushed value is an executable address, but
   * under the VM the pushed fn is an NYIR symbol (a bare
   * nyr_vm_symbol_handle hash for no-capture lambdas, or the code word
   * loaded out of a capture closure) which is non-executable by design.
   * Invoking it as a C function pointer segfaults (the canvas_loop NYIR
   * crash). Run it through ny_native_vm_run_defer_entry, which resolves
   * the handle and installs the entry env at the hidden-environment slot;
   * genuine C addresses still run directly, and anything else is a
   * deterministic error, never a raw indirect call.
   */
  if ((strcmp(symbol, "rt_pop_run_defer") == 0 ||
       strcmp(symbol, "__pop_run_defer") == 0) &&
      arg_count == 0) {
    int64_t fn = 0, env = 0;
    if (!rt_defer_pop_raw(&fn, &env)) {
      if (out)
        *out = 0;
      return true;
    }
    char derr[512];
    derr[0] = '\0';
    if (ny_native_vm_run_defer_entry(ctx, fn, env, derr, sizeof(derr)))
      goto defer_done;
    if (derr[0] != '\0') {
      nyr_set_error(err, err_len, "%s", derr);
      return false;
    }

    if (fn && rt_addr_readable((uintptr_t)fn, 1)) {
      int64_t (*f)(int64_t) = (int64_t (*)(int64_t))(uintptr_t)fn;
      (void)f(env);
      goto defer_done;
    }
    return nyr_set_error(err, err_len,
                             "native NYIR VM: unresolved defer target"),
           false;
  defer_done:
    if (out)
      *out = 0;
    return true;
  }
  if ((strcmp(symbol, "rt_run_defers_to") == 0 ||
       strcmp(symbol, "__run_defers_to") == 0) &&
      arg_count == 1) {
    int64_t target =
        (args && is_int(args[0])) ? (args[0] >> 1) : (args ? args[0] : 0);
    if (target < 0)
      target = 0;
    while (rt_defer_len() > target) {
      int64_t fn = 0, env = 0;
      if (!rt_defer_pop_raw(&fn, &env))
        break;
      char derr[512];
      derr[0] = '\0';
      if (ny_native_vm_run_defer_entry(ctx, fn, env, derr, sizeof(derr)))
        continue;
      if (derr[0] != '\0') {
        nyr_set_error(err, err_len, "%s", derr);
        return false;
      }
      if (fn && rt_addr_readable((uintptr_t)fn, 1)) {
        int64_t (*f)(int64_t) = (int64_t (*)(int64_t))(uintptr_t)fn;
        (void)f(env);
        continue;
      }
      return nyr_set_error(err, err_len,
                               "native NYIR VM: unresolved defer target"),
             false;
    }
    if (out)
      *out = 0;
    return true;
  }
  const ny_native_runtime_callback_abi_decl_t *callback_abi =
      ny_native_runtime_callback_abi_decl(symbol);
  if (callback_abi) {
    if (!ny_native_vm_callback_abi_valid(callback_abi))
      return nyr_set_error(
                 err, err_len,
                 "native NYIR VM: invalid callback ABI declaration for %s",
                 symbol),
             false;
    const size_t expected_args = (size_t)callback_abi->source_arity + 1;
    if (arg_count != expected_args)
      return nyr_set_error(err, err_len,
                               "native NYIR VM: callback ABI mismatch for %s: "
                               "expected %zu arguments, got %zu",
                               symbol, expected_args, arg_count),
             false;
    /*
     * Slot 0 carries the callable; slots 1..15 the arguments. Callback ABIs
     * accept at most source_arity == 15 (validated above), so the array must
     * hold 16 words — the old [5] bound both truncated the memcpy and sent
     * the case 5..15 callers past the end of the stack array.
     */
    int64_t values[16] = {0};
    if (args)
      memcpy(values, args, expected_args * sizeof(*values));
    /*
     * Captured NYIR lambdas are heap closures with an unmarked value. Send
     * those through the VM resolver too: rt_call_anyN would otherwise treat
     * their symbol-handle code word as a native executable address. Keep
     * arbitrary unmarked values and raw C function pointers on the regular
     * runtime path.
     */
    bool vm_closure_callable = !NY_DYNAMIC_CALLABLE_IS(values[0]) &&
                               ny_native_vm_is_closure_value(values[0]);
    if (args && (callback_abi->vm_resolve_unmarked_callable ||
                 NY_DYNAMIC_CALLABLE_IS(values[0]) || vm_closure_callable)) {
      const int64_t *callback_args =
          callback_abi->source_arity ? &values[1] : NULL;
      if (ny_native_vm_dynamic_callback(ctx, values[0], callback_args,
                                        callback_abi->source_arity, out, err,
                                        err_len))
        return true;
      /*
       * The resolver diagnosed the callback (argument mismatch, eval
       * failure, ...): keep its error. The "unresolved" report is only for
       * a callable word that matches no known function.
       */
      if (err && err[0])
        return false;
      if ((uintptr_t)NY_DYNAMIC_CALLABLE_DECODE(values[0]) < 4096)
        return nyr_set_error(
                   err, err_len,
                   "native NYIR VM: unresolved callback target "
                   "(value=0x%016" PRIx64 ", decoded=0x%016" PRIxPTR
                   ", arity=%u)",
                   (uint64_t)values[0],
                   (uintptr_t)NY_DYNAMIC_CALLABLE_DECODE(values[0]),
                   callback_abi->source_arity),
               false;
    }
    if (!out)
      return true;
    switch (callback_abi->source_arity) {
    case 0:
      *out = callback_abi->call.call0(values[0]);
      return true;
    case 1:
      *out = callback_abi->call.call1(values[0], values[1]);
      return true;
    case 2:
      *out = callback_abi->call.call2(values[0], values[1], values[2]);
      return true;
    case 3:
      *out =
          callback_abi->call.call3(values[0], values[1], values[2], values[3]);
      return true;
    case 4:
      *out = callback_abi->call.call4(values[0], values[1], values[2],
                                      values[3], values[4]);
      return true;
    case 5:
      *out = callback_abi->call.call5(values[0], values[1], values[2],
                                      values[3], values[4], values[5]);
      return true;
    case 6:
      *out =
          callback_abi->call.call6(values[0], values[1], values[2], values[3],
                                   values[4], values[5], values[6]);
      return true;
    case 7:
      *out =
          callback_abi->call.call7(values[0], values[1], values[2], values[3],
                                   values[4], values[5], values[6], values[7]);
      return true;
    case 8:
      *out = callback_abi->call.call8(values[0], values[1], values[2],
                                      values[3], values[4], values[5],
                                      values[6], values[7], values[8]);
      return true;
    case 9:
      *out = callback_abi->call.call9(
          values[0], values[1], values[2], values[3], values[4], values[5],
          values[6], values[7], values[8], values[9]);
      return true;
    case 10:
      *out = callback_abi->call.call10(
          values[0], values[1], values[2], values[3], values[4], values[5],
          values[6], values[7], values[8], values[9], values[10]);
      return true;
    case 11:
      *out = callback_abi->call.call11(
          values[0], values[1], values[2], values[3], values[4], values[5],
          values[6], values[7], values[8], values[9], values[10], values[11]);
      return true;
    case 12:
      *out = callback_abi->call.call12(
          values[0], values[1], values[2], values[3], values[4], values[5],
          values[6], values[7], values[8], values[9], values[10], values[11],
          values[12]);
      return true;
    case 13:
      *out = callback_abi->call.call13(
          values[0], values[1], values[2], values[3], values[4], values[5],
          values[6], values[7], values[8], values[9], values[10], values[11],
          values[12], values[13]);
      return true;
    case 14:
      *out = callback_abi->call.call14(
          values[0], values[1], values[2], values[3], values[4], values[5],
          values[6], values[7], values[8], values[9], values[10], values[11],
          values[12], values[13], values[14]);
      return true;
    case 15:
      *out = callback_abi->call.call15(
          values[0], values[1], values[2], values[3], values[4], values[5],
          values[6], values[7], values[8], values[9], values[10], values[11],
          values[12], values[13], values[14], values[15]);
      return true;
    default:
      return nyr_set_error(err, err_len,
                               "native NYIR VM: unsupported callback arity %u",
                               callback_abi->source_arity),
             false;
    }
  }
  if (strcmp(symbol, "rt_malloc") == 0 && arg_count == 1) {
    int64_t p = rt_malloc(args ? args[0] : 0);
    if (!p)
      return nyr_set_error(err, err_len, "native NYIR VM: malloc failed"),
             false;
    if (out)
      *out = p;
    return true;
  }
  if (strcmp(symbol, "rt_free") == 0 && arg_count == 1) {
    rt_free(args ? args[0] : 0);
    if (out)
      *out = 0;
    return true;
  }
  size_t i = ny_native_vm_find_symbol(ctx, symbol);
  if (i != SIZE_MAX) {
    nyr_func_t *callee = &ctx->funcs[i];
    size_t local_count = nyr_local_count(callee);
    if (local_count < arg_count)
      local_count = arg_count;
    int64_t *locals = ny_native_locals_acquire(ctx->depth + 1, local_count);
    if (local_count && !locals)
      return nyr_set_error(err, err_len, NY_NATIVE_OOM), false;
    for (size_t a = 0; a < arg_count; ++a)
      locals[a] = args ? args[a] : 0;
    nyr_eval_result_t r = {0};
    ctx->depth++;
    bool ok =
        nyr_ir_eval_with_calls(callee, locals, local_count, ctx->max_steps, &r,
                             ny_native_vm_call_resolve, ctx, err, err_len);
    ctx->depth--;
    if (!ok)
      return false;
    ny_native_vm_profile_merge(ctx->profile, &r);
    if (!r.returned) {
      nyr_eval_result_free(&r);
      return nyr_set_error(err, err_len,
                               "native NYIR VM: callee '%s' did not return",
                               ctx->names[i] ? ctx->names[i] : symbol),
             false;
    }
    if (out)
      *out = r.result;
    nyr_eval_result_free(&r);
    return true;
  }
  void *sym_ptr = ny_process_symbol(symbol);
  if (sym_ptr) {
    typedef int64_t (*fn0_t)(void);
    typedef int64_t (*fn1_t)(int64_t);
    typedef int64_t (*fn2_t)(int64_t, int64_t);
    typedef int64_t (*fn3_t)(int64_t, int64_t, int64_t);
    typedef int64_t (*fn4_t)(int64_t, int64_t, int64_t, int64_t);
    typedef int64_t (*fn5_t)(int64_t, int64_t, int64_t, int64_t, int64_t);
    typedef int64_t (*fn6_t)(int64_t, int64_t, int64_t, int64_t, int64_t,
                             int64_t);
    typedef int64_t (*fn7_t)(int64_t, int64_t, int64_t, int64_t, int64_t,
                             int64_t, int64_t);
    typedef int64_t (*fn8_t)(int64_t, int64_t, int64_t, int64_t, int64_t,
                             int64_t, int64_t, int64_t);
    typedef int64_t (*fn9_t)(int64_t, int64_t, int64_t, int64_t, int64_t,
                             int64_t, int64_t, int64_t, int64_t);
    typedef int64_t (*fn10_t)(int64_t, int64_t, int64_t, int64_t, int64_t,
                              int64_t, int64_t, int64_t, int64_t, int64_t);
    typedef int64_t (*fn11_t)(int64_t, int64_t, int64_t, int64_t, int64_t,
                              int64_t, int64_t, int64_t, int64_t, int64_t,
                              int64_t);
    typedef int64_t (*fn12_t)(int64_t, int64_t, int64_t, int64_t, int64_t,
                              int64_t, int64_t, int64_t, int64_t, int64_t,
                              int64_t, int64_t);
    typedef int64_t (*fn13_t)(int64_t, int64_t, int64_t, int64_t, int64_t,
                              int64_t, int64_t, int64_t, int64_t, int64_t,
                              int64_t, int64_t, int64_t);
    typedef int64_t (*fn14_t)(int64_t, int64_t, int64_t, int64_t, int64_t,
                              int64_t, int64_t, int64_t, int64_t, int64_t,
                              int64_t, int64_t, int64_t, int64_t);
    typedef int64_t (*fn15_t)(int64_t, int64_t, int64_t, int64_t, int64_t,
                              int64_t, int64_t, int64_t, int64_t, int64_t,
                              int64_t, int64_t, int64_t, int64_t, int64_t);
    typedef int64_t (*fn16_t)(
        int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
        int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t);
    int64_t res = 0;
#define NY_VM_ARG(n) (args ? args[n] : 0)
    switch (arg_count) {
    case 0:
      res = ((fn0_t)sym_ptr)();
      break;
    case 1:
      res = ((fn1_t)sym_ptr)(args ? args[0] : 0);
      break;
    case 2:
      res = ((fn2_t)sym_ptr)(args ? args[0] : 0, args ? args[1] : 0);
      break;
    case 3:
      res = ((fn3_t)sym_ptr)(args ? args[0] : 0, args ? args[1] : 0,
                             args ? args[2] : 0);
      break;
    case 4:
      res = ((fn4_t)sym_ptr)(args ? args[0] : 0, args ? args[1] : 0,
                             args ? args[2] : 0, args ? args[3] : 0);
      break;
    case 5:
      res = ((fn5_t)sym_ptr)(args ? args[0] : 0, args ? args[1] : 0,
                             args ? args[2] : 0, args ? args[3] : 0,
                             args ? args[4] : 0);
      break;
    case 6:
      res = ((fn6_t)sym_ptr)(NY_VM_ARG(0), NY_VM_ARG(1), NY_VM_ARG(2),
                             NY_VM_ARG(3), NY_VM_ARG(4), NY_VM_ARG(5));
      break;
    case 7:
      res = ((fn7_t)sym_ptr)(NY_VM_ARG(0), NY_VM_ARG(1), NY_VM_ARG(2),
                             NY_VM_ARG(3), NY_VM_ARG(4), NY_VM_ARG(5),
                             NY_VM_ARG(6));
      break;
    case 8:
      res = ((fn8_t)sym_ptr)(NY_VM_ARG(0), NY_VM_ARG(1), NY_VM_ARG(2),
                             NY_VM_ARG(3), NY_VM_ARG(4), NY_VM_ARG(5),
                             NY_VM_ARG(6), NY_VM_ARG(7));
      break;
    case 9:
      res = ((fn9_t)sym_ptr)(NY_VM_ARG(0), NY_VM_ARG(1), NY_VM_ARG(2),
                             NY_VM_ARG(3), NY_VM_ARG(4), NY_VM_ARG(5),
                             NY_VM_ARG(6), NY_VM_ARG(7), NY_VM_ARG(8));
      break;
    case 10:
      res = ((fn10_t)sym_ptr)(
          NY_VM_ARG(0), NY_VM_ARG(1), NY_VM_ARG(2), NY_VM_ARG(3), NY_VM_ARG(4),
          NY_VM_ARG(5), NY_VM_ARG(6), NY_VM_ARG(7), NY_VM_ARG(8), NY_VM_ARG(9));
      break;
    case 11:
      res = ((fn11_t)sym_ptr)(NY_VM_ARG(0), NY_VM_ARG(1), NY_VM_ARG(2),
                              NY_VM_ARG(3), NY_VM_ARG(4), NY_VM_ARG(5),
                              NY_VM_ARG(6), NY_VM_ARG(7), NY_VM_ARG(8),
                              NY_VM_ARG(9), NY_VM_ARG(10));
      break;
    case 12:
      res = ((fn12_t)sym_ptr)(NY_VM_ARG(0), NY_VM_ARG(1), NY_VM_ARG(2),
                              NY_VM_ARG(3), NY_VM_ARG(4), NY_VM_ARG(5),
                              NY_VM_ARG(6), NY_VM_ARG(7), NY_VM_ARG(8),
                              NY_VM_ARG(9), NY_VM_ARG(10), NY_VM_ARG(11));
      break;
    case 13:
      res = ((fn13_t)sym_ptr)(
          NY_VM_ARG(0), NY_VM_ARG(1), NY_VM_ARG(2), NY_VM_ARG(3), NY_VM_ARG(4),
          NY_VM_ARG(5), NY_VM_ARG(6), NY_VM_ARG(7), NY_VM_ARG(8), NY_VM_ARG(9),
          NY_VM_ARG(10), NY_VM_ARG(11), NY_VM_ARG(12));
      break;
    case 14:
      res = ((fn14_t)sym_ptr)(
          NY_VM_ARG(0), NY_VM_ARG(1), NY_VM_ARG(2), NY_VM_ARG(3), NY_VM_ARG(4),
          NY_VM_ARG(5), NY_VM_ARG(6), NY_VM_ARG(7), NY_VM_ARG(8), NY_VM_ARG(9),
          NY_VM_ARG(10), NY_VM_ARG(11), NY_VM_ARG(12), NY_VM_ARG(13));
      break;
    case 15:
      res = ((fn15_t)sym_ptr)(NY_VM_ARG(0), NY_VM_ARG(1), NY_VM_ARG(2),
                              NY_VM_ARG(3), NY_VM_ARG(4), NY_VM_ARG(5),
                              NY_VM_ARG(6), NY_VM_ARG(7), NY_VM_ARG(8),
                              NY_VM_ARG(9), NY_VM_ARG(10), NY_VM_ARG(11),
                              NY_VM_ARG(12), NY_VM_ARG(13), NY_VM_ARG(14));
      break;
    case 16:
      res = ((fn16_t)sym_ptr)(
          NY_VM_ARG(0), NY_VM_ARG(1), NY_VM_ARG(2), NY_VM_ARG(3), NY_VM_ARG(4),
          NY_VM_ARG(5), NY_VM_ARG(6), NY_VM_ARG(7), NY_VM_ARG(8), NY_VM_ARG(9),
          NY_VM_ARG(10), NY_VM_ARG(11), NY_VM_ARG(12), NY_VM_ARG(13),
          NY_VM_ARG(14), NY_VM_ARG(15));
      break;
    default:
#undef NY_VM_ARG
      return nyr_set_error(err, err_len,
                               "native NYIR VM: dlsym call arity %zu exceeds "
                               "runtime ABI limit 16",
                               arg_count),
             false;
    }
#undef NY_VM_ARG
    if (out)
      *out = res;
    return true;
  }
  return nyr_set_error(err, err_len,
                           "native NYIR VM: unresolved call target '%s'",
                           symbol),
         false;
}

/*
 * The wrapper env converts panics raised inside ordinary C runtime calls
 * into the language-panic marker: the NYIR VM evaluator unwinds that marker
 * to the enclosing try's catch arm (machine code instead lets rt_panic
 * longjmp straight at the try's jmp_buf — an interpreter cannot resume a
 * dead setjmp frame, so the unwind is a protocol, not a jump).
 */
static bool ny_native_vm_call_resolve(void *opaque, const char *symbol,
                                      const int64_t *args, size_t arg_count,
                                      int64_t *out, char *err, size_t err_len) {
  /*
   * A compiled rt_panic call IS the language raise (throw, divide-by-zero
   * guards). Record the payload and report the marker — the evaluator's try
   * machinery takes it from here; with no active try the marker propagates
   * to the driver as a runtime error.
   */
  if (symbol &&
      (strcmp(symbol, "rt_panic") == 0 || strcmp(symbol, "__panic") == 0)) {
    rt_set_panic_val(arg_count > 0 ? args[0] : 0);
    nyr_set_error(err, err_len, NYIR_VM_LANGUAGE_PANIC_ERR);
    return false;
  }
  jmp_buf env;
  rt_set_panic_env(&env);
  if (setjmp(env) != 0) {
    rt_clear_panic_env();
    nyr_set_error(err, err_len, NYIR_VM_LANGUAGE_PANIC_ERR);
    return false;
  }
  bool ok = ny_native_vm_call_resolve_impl(opaque, symbol, args, arg_count, out,
                                           err, err_len);
  rt_clear_panic_env();
  return ok;
}

/*
 * Convert the interpreter's private raise marker into the same user-visible
 * panic text used by the direct NYIR VM execution path.  Profile collection
 * also executes rt_main, so it must normalize uncaught raises before the
 * compiler pipeline reports an evaluation failure.
 */
static void ny_native_vm_normalize_panic(char *err, size_t err_len) {
  if (!err || err_len == 0 || strcmp(err, NYIR_VM_LANGUAGE_PANIC_ERR) != 0)
    return;
  int64_t panic_value = rt_get_panic_val();
  if (panic_value && rt_native_is_str(panic_value)) {
    int64_t message_len = rt_cstr_len(panic_value);
    size_t copy_len = message_len > 0 ? (size_t)message_len : 0;
    if (copy_len >= err_len)
      copy_len = err_len - 1;
    memcpy(err, (const void *)(uintptr_t)panic_value, copy_len);
    err[copy_len] = '\0';
  } else {
    nyr_set_error(err, err_len, "runtime panicked");
  }
}

bool ny_lang_run_ir(nyr_func_t *rt_main, nyr_func_t *funcs,
                    const char *const *names, size_t count,
                    const ny_options *opt, const char *name, char *err,
                    size_t err_len) {
  size_t local_count = nyr_local_count(rt_main);
  int64_t *locals = ny_native_locals_acquire(0, local_count);
  if (local_count && !locals) {
    nyr_set_error(err, err_len, NY_NATIVE_OOM);
    return false;
  }
  ny_native_vm_call_ctx_t ctx = {.funcs = funcs,
                                 .names = names,
                                 .count = count,
                                 .recursion_limit =
                                     ny_native_vm_recursion_limit(opt),
                                 .max_steps = ny_native_vm_max_steps(opt)};
  nyr_eval_result_t result = {0};
  nyr_eval_result_t nested_profile = {0};
  ctx.profile = &nested_profile;
  bool ok = nyr_ir_eval_with_calls(rt_main, locals, local_count,
                                 ny_native_vm_max_steps(opt), &result,
                                 ny_native_vm_call_resolve, &ctx, err, err_len);
  if (!ok) {
    /*
     * An uncaught language raise surfaces as the internal marker; report
     * the user-facing panic message instead.
     */
    ny_native_vm_normalize_panic(err, err_len);
    return false;
  }
  ny_native_vm_profile_merge(&nested_profile, &result);
  ny_native_vm_profile_take_detail(&nested_profile, &result);
  nested_profile.returned = result.returned;
  nested_profile.result = result.result;
  bool wrote =
      ny_native_write_eval_result(opt, &nested_profile, name, err, err_len);
  nyr_eval_result_free(&result);
  nyr_eval_result_free(&nested_profile);
  return wrote;
}

bool ny_lang_eval_ir_value(nyr_func_t *rt_main, nyr_func_t *funcs,
                           const char *const *names, size_t count,
                           const ny_options *opt, nyr_eval_result_t *out,
                           char *err, size_t err_len) {
  if (!rt_main || !out) {
    nyr_set_error(err, err_len, "native oracle: missing NYIR entry");
    return false;
  }
  size_t local_count = nyr_local_count(rt_main);
  int64_t *locals = ny_native_locals_acquire(0, local_count);
  if (local_count && !locals) {
    nyr_set_error(err, err_len, "native oracle: out of memory");
    return false;
  }
  ny_native_vm_call_ctx_t ctx = {.funcs = funcs,
                                 .names = names,
                                 .count = count,
                                 .recursion_limit =
                                     ny_native_vm_recursion_limit(opt),
                                 .max_steps = ny_native_vm_max_steps(opt)};
  nyr_eval_result_t top = {0};
  nyr_eval_result_t nested = {0};
  ctx.profile = &nested;
  bool ok = nyr_ir_eval_with_calls(rt_main, locals, local_count,
                                 ny_native_vm_max_steps(opt), &top,
                                 ny_native_vm_call_resolve, &ctx, err, err_len);
  if (!ok) {
    ny_native_vm_normalize_panic(err, err_len);
    return false;
  }
  ny_native_vm_profile_merge(&nested, &top);
  ny_native_vm_profile_take_detail(&nested, &top);
  nested.returned = top.returned;
  nested.result = top.result;
  *out = nested;
  nyr_eval_result_free(&top);
  return true;
}

bool ny_lang_collect_vm_profile(nyr_func_t *rt_main, nyr_func_t *funcs,
                                const char *const *names, size_t count,
                                const ny_options *opt,
                                nyr_eval_result_t *profile, char *err,
                                size_t err_len) {
  if (!rt_main || !profile)
    return false;
  memset(profile, 0, sizeof(*profile));
  size_t local_count = nyr_local_count(rt_main);
  int64_t *locals = ny_native_locals_acquire(0, local_count);
  if (local_count && !locals) {
    nyr_set_error(err, err_len,
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
  nyr_eval_result_t top = {0};
  bool ok = nyr_ir_eval_with_calls(rt_main, locals, local_count,
                                 ny_native_vm_max_steps(opt), &top,
                                 ny_native_vm_call_resolve, &ctx, err, err_len);
  if (!ok) {
    ny_native_vm_normalize_panic(err, err_len);
    return false;
  }
  ny_native_vm_profile_merge(profile, &top);
  ny_native_vm_profile_take_detail(profile, &top);
  profile->returned = top.returned;
  profile->result = top.result;
  nyr_eval_result_free(&top);
  return true;
}

static bool nyr_read_u16le(FILE *in, uint16_t *out) {
  unsigned char bytes[2];
  if (!in || !out || fread(bytes, 1, sizeof(bytes), in) != sizeof(bytes))
    return false;
  *out = (uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8);
  return true;
}

static bool nyr_read_u32le(FILE *in, uint32_t *out) {
  unsigned char bytes[4];
  if (!in || !out || fread(bytes, 1, sizeof(bytes), in) != sizeof(bytes))
    return false;
  *out = (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
         ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
  return true;
}

static bool nyr_load_bundle_member(FILE *in, nyr_func_t *out, char *name,
                                   size_t name_len, char *err, size_t err_len) {
  uint32_t bytes = 0;
  if (!nyr_read_u32le(in, &bytes) || bytes == 0 || bytes > 64u * 1024u * 1024u)
    return nyr_set_error(err, err_len,
                             "native NYIR bundle: invalid member size"),
           false;
  unsigned char *blob = malloc(bytes);
  if (!blob)
    return nyr_set_error(err, err_len, NY_NATIVE_BUNDLE_OOM), false;
  bool ok = fread(blob, 1, bytes, in) == bytes;
  FILE *tmp = ok ? tmpfile() : NULL;
  if (!tmp)
    ok = false;
  if (ok && fwrite(blob, 1, bytes, tmp) != bytes)
    ok = false;
  if (ok && fseek(tmp, 0, SEEK_SET) != 0)
    ok = false;
  if (ok)
    ok = nyr_load_binary(tmp, out, name, name_len, err, err_len);
  if (ok && fgetc(tmp) != EOF) {
    nyr_set_error(err, err_len,
                      "native NYIR bundle: trailing data in member");
    ok = false;
  }
  if (tmp)
    fclose(tmp);
  free(blob);
  if (!ok && err && err_len && !err[0])
    nyr_set_error(err, err_len, "native NYIR bundle: malformed member");
  return ok;
}

static bool ny_native_eval_ir_bundle(FILE *in, const ny_options *opt, char *err,
                                     size_t err_len) {
  uint16_t version = 0, flags = 0;
  uint32_t members = 0;
  if (!nyr_read_u16le(in, &version) || !nyr_read_u16le(in, &flags) ||
      !nyr_read_u32le(in, &members) || version != 1 || flags != 0 ||
      members == 0 || members > NYR_BUNDLE_MAX_FUNCS + 1)
    return nyr_set_error(
               err, err_len,
               "native NYIR bundle: unsupported or malformed header"),
           false;
  nyr_func_t entry = {0};
  size_t function_cap = (size_t)members - 1;
  nyr_func_t *funcs =
      function_cap ? calloc(function_cap, sizeof(*funcs)) : NULL;
  char **names = function_cap ? calloc(function_cap, sizeof(*names)) : NULL;
  if (function_cap && (!funcs || !names)) {
    free(funcs);
    free(names);
    return nyr_set_error(err, err_len, NY_NATIVE_BUNDLE_OOM), false;
  }
  char entry_name[128] = {0};
  size_t loaded = 0;
  bool ok = nyr_load_bundle_member(in, &entry, entry_name, sizeof(entry_name),
                                   err, err_len) &&
            strcmp(entry_name, "rt_main") == 0;
  if (!ok && err && err_len && !err[0])
    nyr_set_error(err, err_len,
                      "native NYIR bundle: first member must be rt_main");
  for (uint32_t i = 1; ok && i < members; ++i) {
    char name[128] = {0};
    ok = nyr_load_bundle_member(in, &funcs[loaded], name, sizeof(name), err,
                                err_len);
    if (ok && (!name[0] || strcmp(name, "rt_main") == 0)) {
      nyr_set_error(err, err_len,
                        "native NYIR bundle: invalid function name");
      ok = false;
    }
    for (size_t j = 0; ok && j < loaded; ++j) {
      if (strcmp(name, names[j]) == 0) {
        nyr_set_error(err, err_len,
                          "native NYIR bundle: duplicate function name '%s'",
                          name);
        ok = false;
      }
    }
    if (ok) {
      names[loaded] = ny_strdup(name);
      if (!names[loaded]) {
        nyr_set_error(err, err_len, NY_NATIVE_BUNDLE_OOM);
        ok = false;
      } else {
        loaded++;
      }
    }
  }
  if (ok && fgetc(in) != EOF) {
    nyr_set_error(err, err_len,
                      "native NYIR bundle: trailing data after final member");
    ok = false;
  }
  if (ok)
    ok = ny_lang_run_ir(&entry, funcs, (const char **)names, loaded, opt,
                        entry_name, err, err_len);
  nyr_ir_func_free(&entry);
  for (size_t i = 0; i < loaded; ++i) {
    nyr_ir_func_free(&funcs[i]);
    free(names[i]);
  }
  free(funcs);
  free(names);
  return ok;
}

bool ny_lang_eval_ir_binary_file(const char *path, const ny_options *opt,
                                 char *err, size_t err_len) {
  if (!path || !*path) {
    nyr_set_error(err, err_len,
                      "native NYIR VM: missing binary input path");
    return false;
  }
  FILE *in = fopen(path, "rb");
  if (!in) {
    nyr_set_error(err, err_len, "native NYIR VM: failed to open %s: %s",
                      path, strerror(errno));
    return false;
  }
  char magic[4];
  if (fread(magic, 1, sizeof(magic), in) != sizeof(magic)) {
    fclose(in);
    nyr_set_error(err, err_len, "native NYIR VM: malformed binary input");
    return false;
  }
  if (memcmp(magic, "NYIP", 4) == 0) {
    bool bundle_ok = ny_native_eval_ir_bundle(in, opt, err, err_len);
    fclose(in);
    return bundle_ok;
  }
  if (memcmp(magic, "NYIR", 4) != 0 || fseek(in, 0, SEEK_SET) != 0) {
    fclose(in);
    nyr_set_error(err, err_len, "native NYIR VM: malformed binary input");
    return false;
  }
  nyr_func_t f = {0};
  char name[128] = {0};
  bool ok = nyr_load_binary(in, &f, name, sizeof(name), err, err_len);
  if (ok && fgetc(in) != EOF) {
    nyr_set_error(err, err_len,
                      "native NYIR load: trailing data after function");
    ok = false;
  }
  fclose(in);
  if (!ok) {
    nyr_ir_func_free(&f);
    return false;
  }
  ok =
      ny_native_eval_ir_func(&f, opt, name[0] ? name : "rt_main", err, err_len);
  nyr_ir_func_free(&f);
  return ok;
}
