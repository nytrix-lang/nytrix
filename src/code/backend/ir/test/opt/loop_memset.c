/*
 * A counted zero-fill may become one raw memset only when its memory accesses
 * cannot trap partway through the loop. Keep byte and i64 widths distinct.
 */
#include "code/backend/ir/opt/loop.h"
#include "support.h"

#include <inttypes.h>
#include <stdint.h>
#include <string.h>

typedef struct {
  size_t calls;
} memset_ctx_t;

static bool resolve_memset(void *opaque, const char *symbol,
                           const int64_t *args, size_t argc, int64_t *result,
                           char *err, size_t err_len) {
  memset_ctx_t *ctx = opaque;
  if (!symbol || strcmp(symbol, "rt_memset_raw") != 0 || argc != 3) {
    snprintf(err, err_len, "unexpected NYIR call: %s",
             symbol ? symbol : "<null>");
    return false;
  }
  memset((void *)(uintptr_t)args[0], (unsigned char)args[1], (size_t)args[2]);
  *result = args[0];
  ctx->calls++;
  return true;
}

static int emit(nyr_func_t *f, nyr_inst_t in) { return nyr_ir_emit(f, in); }

static int address_local(nyr_func_t *f, int slot) {
  return emit(f, (nyr_inst_t){.op = NYIR_ADDR_LOCAL,
                               .dst = -1,
                               .a = -1,
                               .b = -1,
                               .c = -1,
                               .d = -1,
                               .e = -1,
                               .f = -1,
                               .imm = slot});
}

static int load_i64(nyr_func_t *f, int addr, unsigned flags) {
  return emit(f, (nyr_inst_t){.op = NYIR_LOAD_I64,
                               .dst = -1,
                               .a = addr,
                               .b = -1,
                               .c = -1,
                               .d = -1,
                               .e = -1,
                               .f = -1,
                               .flags = flags});
}

static void store_i64(nyr_func_t *f, int addr, int value, unsigned flags) {
  emit(f, (nyr_inst_t){.op = NYIR_STORE_I64,
                        .dst = -1,
                        .a = addr,
                        .b = -1,
                        .c = value,
                        .d = -1,
                        .e = -1,
                        .f = -1,
                        .flags = flags});
}

static bool build_fill(nyr_func_t *f, int64_t elem, int64_t fill,
                       int64_t byte_len) {
  nyr_opt_begin(f);
  int base = emit(f, (nyr_inst_t){.op = NYIR_ALLOCA,
                                   .dst = -1,
                                   .a = -1,
                                   .b = -1,
                                   .c = -1,
                                   .d = -1,
                                   .e = -1,
                                   .f = -1,
                                   .imm = 8 * elem});
  int zero = nyr_opt_const(f, 0);
  int one = nyr_opt_const(f, 1);
  int last_offset = nyr_opt_const(f, 7 * elem);
  int initial = nyr_opt_const(f, 0x5a);
  int init_addr_last = nyr_opt_binop(f, NYIR_ADD_I64, base, last_offset);
  unsigned mem_flags = elem == 1 ? NYIR_INST_F_MEM_BYTE : 0;
  store_i64(f, base, initial, mem_flags);
  store_i64(f, init_addr_last, initial, mem_flags);

  int iv_slot = address_local(f, 0);
  store_i64(f, iv_slot, zero, 0);
  nyr_opt_br(f, 10);

  nyr_opt_label(f, 10);
  int iv = load_i64(f, iv_slot, 0);
  int limit = nyr_opt_const(f, 8);
  int cond = nyr_opt_cmp(f, iv, limit, NYIR_CMP_LT);
  nyr_opt_br_if(f, cond, 11);
  nyr_opt_br(f, 12);

  nyr_opt_label(f, 11);
  int scale = nyr_opt_const(f, elem);
  int offset = nyr_opt_binop(f, NYIR_MUL_I64, iv, scale);
  int addr = nyr_opt_binop(f, NYIR_ADD_I64, base, offset);
  if (byte_len >= 0) {
    emit(f, (nyr_inst_t){.op = NYIR_BOUNDS_CHECK,
                          .dst = -1,
                          .a = base,
                          .b = offset,
                          .c = -1,
                          .d = -1,
                          .e = -1,
                          .f = -1,
                          .imm = byte_len});
  }
  int value = nyr_opt_const(f, fill);
  store_i64(f, addr, value, mem_flags);
  int current_iv = load_i64(f, iv_slot, 0);
  int next_iv = nyr_opt_binop(f, NYIR_ADD_I64, current_iv, one);
  store_i64(f, iv_slot, next_iv, 0);
  nyr_opt_br(f, 10);

  nyr_opt_label(f, 12);
  int first = load_i64(f, base, mem_flags);
  int last = load_i64(f, init_addr_last, mem_flags);
  int sum = nyr_opt_binop(f, NYIR_ADD_I64, first, last);
  nyr_opt_ret(f, sum);
  return nyr_opt_verify(f, "loop-memset-before");
}

static bool evaluate(nyr_func_t *f, bool want_success, int64_t expected,
                     size_t expected_calls) {
  int64_t locals[4] = {0};
  nyr_eval_result_t result = {0};
  memset_ctx_t ctx = {0};
  char err[256] = {0};
  bool ok = nyr_ir_eval_with_calls(f, locals, 4, 500, &result, resolve_memset,
                                 &ctx, err, sizeof(err));
  if (ok != want_success || (ok && result.result != expected) ||
      ctx.calls != expected_calls) {
    fprintf(stderr,
            "[loop-memset] mismatch success=%d result=%" PRId64
            " calls=%zu error=%s\n",
            ok, result.result, ctx.calls, err);
    nyr_eval_result_free(&result);
    return false;
  }
  nyr_eval_result_free(&result);
  return true;
}

static bool has_raw_memset(const nyr_func_t *f) {
  for (size_t i = 0; i < f->len; ++i)
    if (f->data[i].op == NYIR_CALL && f->data[i].symbol &&
        strcmp(f->data[i].symbol, "rt_memset_raw") == 0)
      return true;
  return false;
}

static int fail_stage(nyr_func_t *f, const char *stage) {
  fprintf(stderr, "[loop-memset] failed at %s\n", stage);
  if (f)
    nyr_dump(stderr, f, "loop-memset-failure");
  return 1;
}

int main(void) {
  nyr_func_t f;
  const int64_t widths[] = {8, 1};
  for (size_t i = 0; i < sizeof(widths) / sizeof(widths[0]); ++i) {
    if (!build_fill(&f, widths[i], 0, -1))
      return fail_stage(&f, "build valid fill");
    if (!evaluate(&f, true, 0, 0))
      return fail_stage(&f, "evaluate before optimization");
    if (!nyr_loop_memset(&f))
      return fail_stage(&f, "run transform");
    if (!nyr_opt_verify(&f, "loop-memset-after"))
      return fail_stage(&f, "verify after transform");
    if (!has_raw_memset(&f))
      return fail_stage(&f, "valid zero-fill did not become raw memset");
    if (!evaluate(&f, true, 0, 1))
      return fail_stage(&f, "evaluate transformed fill");
    nyr_ir_func_free(&f);
  }

  /*
   * Nonzero fills remain stores, preserving the exact byte value.
   */
  if (!build_fill(&f, 8, 1, -1))
    return fail_stage(&f, "build nonzero fill");
  if (!evaluate(&f, true, 2, 0))
    return fail_stage(&f, "evaluate nonzero fill before transform");
  if (!nyr_loop_memset(&f))
    return fail_stage(&f, "run transform on nonzero fill");
  if (has_raw_memset(&f))
    return fail_stage(&f, "nonzero fill became raw memset");
  if (!evaluate(&f, true, 2, 0))
    return fail_stage(&f, "evaluate nonzero fill after transform");
  nyr_ir_func_free(&f);

  /*
   * Bounds failures may occur after earlier stores; do not bulk-hoist them.
   */
  if (!build_fill(&f, 8, 0, 0))
    return fail_stage(&f, "build trapping fill");
  if (!nyr_loop_memset(&f))
    return fail_stage(&f, "run transform on trapping fill");
  if (has_raw_memset(&f))
    return fail_stage(&f, "trapping fill became raw memset");
  if (!evaluate(&f, false, 0, 0))
    return fail_stage(&f, "preserve trapping fill");
  nyr_ir_func_free(&f);
  puts("[loop-memset] raw fill, byte width, nonzero fallback, and trap "
       "preserved");
  return 0;
}
