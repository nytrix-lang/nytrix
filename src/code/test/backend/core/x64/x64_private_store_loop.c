/*
 * Private spill homes stay live across payload stores and fixed-register math.
 */
#include "code/backend/mach/object.h"
#include "opt.h"

static int64_t private_store_input[64];
static uint64_t private_store_output[64];
static int64_t private_store_count;

static int check(void) {
#if !defined(__x86_64__)
  return 0;
#else
  typedef int64_t(__attribute__((sysv_abi)) * kernel_t)(void);
  nyr_func_t ir;
  nyr_opt_begin(&ir);
  nyr_opt_label(&ir, 1);
  int input = nyr_opt_const(&ir, (int64_t)(uintptr_t)private_store_input);
  int output = nyr_opt_const(&ir, (int64_t)(uintptr_t)private_store_output);
  int count_address =
      nyr_opt_const(&ir, (int64_t)(uintptr_t)&private_store_count);
  int count = nyr_opt_emit_abc(&ir, NYIR_LOAD_I64, count_address, -1, -1);
  int zero = nyr_opt_const(&ir, 0);
  int one = nyr_opt_const(&ir, 1);
  int width = nyr_opt_const(&ir, 8);
  int divisor = nyr_opt_const(&ir, 3);
  int seeds[8], states[8], next_states[8];
  size_t phis[8];
  for (int j = 0; j < 8; ++j)
    seeds[j] = nyr_opt_const(&ir, j + 1);
  nyr_opt_br(&ir, 2);
  nyr_opt_label(&ir, 2);
  size_t index_phi = ir.len;
  int index = ir.next_value;
  nyr_opt_phi(&ir, 1, zero, 3, zero);
  for (int j = 0; j < 8; ++j) {
    phis[j] = ir.len;
    states[j] = ir.next_value;
    nyr_opt_phi(&ir, 1, seeds[j], 3, seeds[j]);
  }
  int cond = nyr_opt_cmp(&ir, index, count, NYIR_CMP_LT);
  nyr_opt_br_if(&ir, cond, 3);
  nyr_opt_br(&ir, 4);
  nyr_opt_label(&ir, 3);
  int offset = nyr_opt_binop(&ir, NYIR_MUL_I64, index, width);
  int source = nyr_opt_binop(&ir, NYIR_ADD_I64, input, offset);
  int loaded = nyr_opt_emit_abc(&ir, NYIR_LOAD_I64, source, -1, -1);
  int quotient = nyr_opt_binop(&ir, NYIR_DIV_I64, loaded, divisor);
  int stored = zero;
  for (int j = 0; j < 8; ++j) {
    int added = nyr_opt_binop(&ir, NYIR_ADD_I64, states[j], loaded);
    int salt = nyr_opt_binop(&ir, NYIR_ADD_I64, quotient, seeds[j]);
    next_states[j] = nyr_opt_binop(&ir, NYIR_XOR_I64, added, salt);
    stored = nyr_opt_binop(&ir, NYIR_XOR_I64, stored, next_states[j]);
  }
  int destination = nyr_opt_binop(&ir, NYIR_ADD_I64, output, offset);
  nyr_ir_emit(&ir, (nyr_inst_t){.op = NYIR_STORE_I64,
                                .dst = -1,
                                .a = destination,
                                .b = -1,
                                .c = stored,
                                .d = -1,
                                .e = -1,
                                .f = -1});
  int next_index = nyr_opt_binop(&ir, NYIR_ADD_I64, index, one);
  nyr_opt_br(&ir, 2);
  nyr_opt_label(&ir, 4);
  int result = zero;
  for (int j = 0; j < 8; ++j)
    result = nyr_opt_binop(&ir, NYIR_XOR_I64, result, states[j]);
  nyr_opt_ret(&ir, result);
  ir.data[index_phi].phi_incoming[1].value = next_index;
  for (int j = 0; j < 8; ++j)
    ir.data[phis[j]].phi_incoming[1].value = next_states[j];

  ny_options opt = {0};
  opt.native_backend = NY_NATIVE_BACKEND_X86_64;
  opt.native_abi = NY_NATIVE_ABI_SYSV;
  opt.host_triple = "x86_64-linux-gnu";
  const nyr_target_request_t request = nyr_target_request_make(
      opt.native_backend, opt.native_abi, opt.host_triple, opt.opt_size);
  ny_native_target_info_t target = {0};
  nyr_mach_func_t mach = {0};
  nyr_obj_buf_t code = {0};
  nyr_x64_obj_symbol_def_t defs[2] = {0};
  nyr_x64_obj_reloc_t relocs[2] = {0};
  size_t def_count = 0, reloc_count = 0;
  char err[256] = {0};
  bool ok = nyr_opt_verify(&ir, "private-store-loop") &&
            nyr_target_info_init(&target, &request) &&
            nyr_mach_lower_nir(&ir, &mach, target.caps, err, sizeof(err)) &&
            nyr_x64_mach_append_function(
                &code, defs, &def_count, relocs, &reloc_count, &mach, &target,
                "private_store_loop", false, err, sizeof(err));
  int symbol = -1;
  for (size_t i = 0; ok && i < def_count; ++i)
    if (strcmp(defs[i].name, "private_store_loop") == 0)
      symbol = (int)i;
  void *mapping = MAP_FAILED;
  if (ok && symbol >= 0 && reloc_count == 0) {
    mapping = nyr_opt_execmem_alloc(code.len);
    ok = mapping != MAP_FAILED;
  } else {
    ok = false;
  }
  if (ok) {
    memcpy(mapping, code.data, code.len);
    ok = nyr_opt_execmem_protect(mapping, code.len) == 0;
  }
  uint64_t random = UINT64_C(0x123456789abcdef0);
  const int64_t counts[] = {-1, 0, 1, 7, 31, 64};
  for (int trial = 0; ok && trial < 3; ++trial) {
    for (size_t i = 0; i < 64; ++i) {
      random = random * UINT64_C(6364136223846793005) + 1;
      private_store_input[i] = i % 4 == 0   ? INT64_MIN
                               : i % 4 == 1 ? INT64_MAX
                                            : (int64_t)random;
    }
    for (size_t c = 0; ok && c < sizeof(counts) / sizeof(counts[0]); ++c) {
      const uint64_t sentinel = UINT64_C(0xfedcba9876543210);
      uint64_t expected[64], state[8];
      for (int j = 0; j < 8; ++j)
        state[j] = (uint64_t)j + 1;
      for (size_t i = 0; i < 64; ++i)
        expected[i] = private_store_output[i] = sentinel;
      private_store_count = counts[c];
      for (int64_t i = 0; i < counts[c]; ++i) {
        uint64_t value = (uint64_t)private_store_input[i];
        uint64_t q = (uint64_t)(private_store_input[i] / 3);
        expected[i] = 0;
        for (int j = 0; j < 8; ++j) {
          state[j] = (state[j] + value) ^ (q + (uint64_t)j + 1);
          expected[i] ^= state[j];
        }
      }
      uint64_t expected_result = 0;
      for (int j = 0; j < 8; ++j)
        expected_result ^= state[j];
      kernel_t kernel = (kernel_t)((unsigned char *)mapping + defs[symbol].off);
      uint64_t actual = (uint64_t)kernel();
      ok = actual == expected_result &&
           memcmp(expected, private_store_output, sizeof(expected)) == 0;
      if (!ok)
        fprintf(stderr,
                "private store loop: trial=%d count=%lld result=%llu "
                "expected=%llu\n",
                trial, (long long)counts[c], (unsigned long long)actual,
                (unsigned long long)expected_result);
    }
  }
  if (mapping != MAP_FAILED)
    nyr_opt_execmem_free(mapping, code.len);
  nyr_mach_func_free(&mach);
  nyr_obj_free(&code);
  nyr_ir_func_free(&ir);
  if (!ok)
    fprintf(stderr, "private store loop: %s\n", err);
  return ok ? 0 : 1;
#endif
}

int main(void) { return check(); }
