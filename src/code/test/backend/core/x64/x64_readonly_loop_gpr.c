/*
 * Read-only payload loads preserve loop-carried values in native code.
 */
#include "code/backend/mach/object.h"
#include "opt.h"

typedef int64_t (*readonly_fn_t)(const int64_t *, int64_t, int64_t, int64_t);
#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
typedef int64_t (__attribute__((ms_abi)) *readonly_win64_fn_t)(
    const int64_t *, int64_t, int64_t, int64_t);
#endif

static int check(int divisor_value, bool signed_dividend, bool remainder,
                 bool win64) {
#if !defined(__x86_64__)
  return 0;
#else
  nyr_func_t ir;
  nyr_opt_begin(&ir);
  ir.param_count = 4;
  ir.param_types = malloc(4 * sizeof(*ir.param_types));
  if (!ir.param_types)
    return 2;
  ir.param_types[0] = NYIR_PARAM_I64;
  ir.param_types[1] = ir.param_types[2] = NYIR_PARAM_I64;
  ir.param_types[3] = NYIR_PARAM_I64;
  nyr_opt_label(&ir, 1);
  int base = nyr_opt_emit_imm(&ir, NYIR_LOAD_LOCAL, 0);
  int limit = nyr_opt_emit_imm(&ir, NYIR_LOAD_LOCAL, 1);
  int seed = nyr_opt_emit_imm(&ir, NYIR_LOAD_LOCAL, 2);
  int shift_count = nyr_opt_emit_imm(&ir, NYIR_LOAD_LOCAL, 3);
  int zero = nyr_opt_const(&ir, 0);
  int one = nyr_opt_const(&ir, 1);
  int width = nyr_opt_const(&ir, 8);
  int mask = nyr_opt_const(&ir, 7);
  int divisor = nyr_opt_const(&ir, divisor_value);
  nyr_opt_br(&ir, 2);
  nyr_opt_label(&ir, 2);
  size_t iv_phi = ir.len;
  int iv = ir.next_value;
  nyr_opt_phi(&ir, 1, zero, 3, zero);
  size_t sum_phi = ir.len;
  int sum = ir.next_value;
  nyr_opt_phi(&ir, 1, seed, 3, seed);
  int cond = nyr_opt_cmp(&ir, iv, limit, NYIR_CMP_LT);
  nyr_opt_br_if(&ir, cond, 3);
  nyr_opt_br(&ir, 4);
  nyr_opt_label(&ir, 3);
  int offset = nyr_opt_binop(&ir, NYIR_MUL_I64, iv, width);
  int address = nyr_opt_binop(&ir, NYIR_ADD_I64, base, offset);
  int loaded = nyr_opt_emit_abc(&ir, NYIR_LOAD_I64, address, -1, -1);
  int next_sum = nyr_opt_binop(&ir, NYIR_ADD_I64, sum, loaded);
  next_sum = nyr_opt_binop(&ir, NYIR_ADD_I64, next_sum, iv);
  int bounded_iv = nyr_opt_binop(&ir, NYIR_AND_I64, iv, mask);
  int shifted_iv = nyr_opt_binop(&ir, NYIR_SHL_I64, bounded_iv, shift_count);
  int quotient = nyr_opt_binop(&ir, remainder ? NYIR_MOD_I64 : NYIR_DIV_I64,
                              signed_dividend ? loaded : iv, divisor);
  next_sum = nyr_opt_binop(&ir, NYIR_ADD_I64, next_sum, shifted_iv);
  next_sum = nyr_opt_binop(&ir, NYIR_ADD_I64, next_sum, quotient);
  int next_iv = nyr_opt_binop(&ir, NYIR_ADD_I64, iv, one);
  nyr_opt_br(&ir, 2);
  nyr_opt_label(&ir, 4);
  nyr_opt_ret(&ir, sum);
  ir.data[iv_phi].phi_incoming[1].value = next_iv;
  ir.data[sum_phi].phi_incoming[1].value = next_sum;

  ny_options opt = {0};
  opt.native_backend = NY_NATIVE_BACKEND_X86_64;
  opt.native_abi = win64 ? NY_NATIVE_ABI_WIN64 : NY_NATIVE_ABI_SYSV;
  opt.host_triple = win64 ? "x86_64-windows-gnu" : "x86_64-linux-gnu";
  const nyr_target_request_t request = nyr_target_request_make(
      opt.native_backend, opt.native_abi, opt.host_triple, opt.opt_size);
  ny_native_target_info_t target = {0};
  nyr_mach_func_t mach = {0};
  nyr_obj_buf_t code = {0};
  nyr_x64_obj_symbol_def_t defs[2] = {0};
  nyr_x64_obj_reloc_t relocs[2] = {0};
  size_t def_count = 0, reloc_count = 0;
  char err[256] = {0};
  bool ok = nyr_opt_verify(&ir, "x64-readonly-loop-gpr") &&
            nyr_target_info_init(&target, &request) &&
            nyr_mach_lower_nir(&ir, &mach, target.caps, err, sizeof(err)) &&
            nyr_x64_mach_append_function(
                &code, defs, &def_count, relocs, &reloc_count, &mach, &target,
                "readonly_loop_gpr", false, err, sizeof(err));
  int symbol = -1;
  for (size_t i = 0; ok && i < def_count; ++i) {
    if (strcmp(defs[i].name, "readonly_loop_gpr") == 0) {
      symbol = (int)i;
      break;
    }
  }
  void *mapping = MAP_FAILED;
  if (ok && symbol >= 0 && reloc_count == 0) {
    mapping = nyr_opt_execmem_alloc(code.len);
    ok = mapping != MAP_FAILED;
  } else {
    ok = false;
  }
  if (ok && getenv("NYTRIX_TEST_DUMP_CODE")) {
    FILE *dump = fopen(getenv("NYTRIX_TEST_DUMP_CODE"), "wb");
    if (dump) {
      fwrite(code.data, 1, code.len, dump);
      fclose(dump);
    }
  }
  if (ok) {
    memcpy(mapping, code.data, code.len);
    ok = nyr_opt_execmem_protect(mapping, code.len) == 0;
  }
  if (ok) {
    readonly_fn_t fn =
        (readonly_fn_t)((unsigned char *)mapping + defs[symbol].off);
    int64_t values[64];
    uint64_t random = UINT64_C(0x123456789abcdef0);
    for (int trial = 0; ok && trial < 32; ++trial) {
      for (size_t i = 0; i < 64; ++i) {
        random = random * UINT64_C(6364136223846793005) + 1;
        values[i] = (int64_t)random;
      }
      if (trial == 0) {
        const int64_t edges[] = {INT64_MIN, INT64_MIN + 1, -3, -2, -1,
                                 0, 1, 2, 3, INT64_MAX};
        memcpy(values, edges, sizeof(edges));
      }
      for (int64_t count = -1; ok && count <= 64; ++count) {
        uint64_t expected = random;
        for (int64_t i = 0; i < count; ++i) {
          int64_t dividend = signed_dividend ? values[i] : i;
          int64_t quotient = remainder ? dividend % divisor_value
                                      : dividend / divisor_value;
          expected += (uint64_t)values[i] + (uint64_t)i +
                      (((uint64_t)i & 7u) << 3) + (uint64_t)quotient;
        }
        int64_t actual;
#if defined(__GNUC__) || defined(__clang__)
        if (win64) {
          readonly_win64_fn_t win_fn = (readonly_win64_fn_t)(
              (unsigned char *)mapping + defs[symbol].off);
          actual = win_fn(values, count, (int64_t)random, 3);
        } else
#endif
          actual = fn(values, count, (int64_t)random, 3);
        ok = (uint64_t)actual == expected;
        if (!ok)
          fprintf(stderr,
                  "readonly loop: divisor=%d mode=%s dividend=%s trial=%d "
                  "count=%lld expected=%lld result=%lld abi=%s\n",
                  divisor_value, remainder ? "MOD" : "DIV",
                  signed_dividend ? "payload" : "iv", trial, (long long)count,
                  (long long)expected, (long long)actual,
                  win64 ? "win64" : "sysv");
      }
    }
  }
  if (mapping != MAP_FAILED)
    nyr_opt_execmem_free(mapping, code.len);
  nyr_mach_func_free(&mach);
  nyr_obj_free(&code);
  nyr_ir_func_free(&ir);
  if (!ok)
    fprintf(stderr, "x64 object readonly loop GPR: %s\n", err);
  return ok ? 0 : 1;
#endif
}

int main(void) {
  static const struct {
    int divisor;
    bool signed_dividend;
    bool remainder;
  } cases[] = {{3, false, false}, {2, true, false}, {4, true, false},
               {8, true, true},   {2, true, true},  {3, true, false},
               {3, true, true},   {1, true, false},
               {1073741824, true, false}, {INT32_MAX, true, false},
               {-2, true, false}, {-3, true, true}};
  if (getenv("NYTRIX_TEST_CASE")) {
    size_t i = (size_t)atoi(getenv("NYTRIX_TEST_CASE"));
    return i < sizeof(cases) / sizeof(cases[0])
               ? check(cases[i].divisor, cases[i].signed_dividend,
                       cases[i].remainder, false)
               : 2;
  }
  int failed = 0;
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i)
    {
      failed |= check(cases[i].divisor, cases[i].signed_dividend,
                       cases[i].remainder, false);
#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
      failed |= check(cases[i].divisor, cases[i].signed_dividend,
                       cases[i].remainder, true);
#endif
    }
  return failed;
}
