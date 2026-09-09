/*
 * Immediate rematerialization must stop at fixed-register clobbers.
 */
#include "util.h"
#include "code/native/internal.h"
#include "code/ir/machine.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int ny_copy(nyir_func_t *f, int source) {
  return nyir_emit(f, (nyir_inst_t){.op = NYIR_COPY, .dst = -1, .a = source,
                                    .b = -1, .c = -1, .d = -1, .e = -1,
                                    .f = -1});
}

static void ny_store_local(nyir_func_t *f, int value, int slot) {
  nyir_emit(f, (nyir_inst_t){.op = NYIR_STORE_LOCAL, .dst = -1, .a = value,
                             .b = -1, .c = -1, .d = -1, .e = -1, .f = -1,
                             .imm = slot});
}

static int ny_load_local(nyir_func_t *f, int slot) {
  return nyir_emit(f, (nyir_inst_t){.op = NYIR_LOAD_LOCAL, .dst = -1, .a = -1,
                                    .b = -1, .c = -1, .d = -1, .e = -1,
                                    .f = -1, .imm = slot});
}

static void build_boundaries(nyir_func_t *f) {
  ny_begin(f);
  int base = ny_const(f, 40);
  int div_imm = ny_const(f, -13);
  int safe_div = ny_binop(f, NYIR_ADD_I64, base, div_imm);
  int divisor = ny_const(f, 3);
  ny_store_local(f, divisor, 0);
  int variable_divisor = ny_load_local(f, 0);
  int dividend = ny_const(f, 30);
  int quotient = ny_binop(f, NYIR_DIV_I64, dividend, variable_divisor);
  int after_div = ny_binop(f, NYIR_ADD_I64, quotient, div_imm);
  int shift_imm = ny_const(f, -17);
  int safe_shift = ny_binop(f, NYIR_ADD_I64, base, shift_imm);
  int variable_count = ny_load_local(f, 0);
  int shifted = ny_binop(f, NYIR_SHL_I64, quotient, variable_count);
  int after_shift = ny_binop(f, NYIR_ADD_I64, shifted, shift_imm);
  int combined = ny_binop(f, NYIR_ADD_I64, after_div, after_shift);
  (void)safe_div;
  (void)safe_shift;
  ny_ret(f, combined);
}

static void build_call(nyir_func_t *f) {
  ny_begin(f);
  int base = ny_const(f, 40);
  int safe_imm = ny_const(f, -7);
  int safe_copy = ny_copy(f, safe_imm);
  int safe = ny_binop(f, NYIR_ADD_I64, base, safe_copy);
  int after_call_imm = ny_const(f, -11);
  int after_call_copy = ny_copy(f, after_call_imm);
  nyir_emit(f, (nyir_inst_t){.op = NYIR_CALL, .dst = -1, .a = -1, .b = -1,
                             .c = -1, .d = -1, .e = -1, .f = -1, .imm = 0,
                             .symbol = "touch"});
  int after_call = ny_binop(f, NYIR_ADD_I64, base, after_call_copy);
  (void)safe;
  ny_ret(f, after_call);
}

static size_t count_text(const char *text, const char *needle) {
  size_t count = 0;
  size_t len = strlen(needle);
  while ((text = strstr(text, needle)) != NULL) {
    count++;
    text += len;
  }
  return count;
}

static bool verify_machine(const nyir_func_t *f, const char *name) {
  ny_mach_func_t mach = {0};
  char err[256] = {0};
  bool ok = ny_mach_lower_nir(f, &mach, 0, err, sizeof(err)) &&
            ny_mach_verify(&mach, 0, err, sizeof(err));
  if (!ok)
    fprintf(stderr, "[%s] machine verification failed: %s\n", name, err);
  ny_mach_func_free(&mach);
  return ok;
}

static bool emit_text(const nyir_func_t *f, const char *name,
                      ny_native_writer_t *out) {
  static const ny_native_target_info_t target = {
      .target = NY_NATIVE_TARGET_X86_64,
      .abi = NY_NATIVE_ABI_SYSV,
      .object_format = "elf",
      .symbol_prefix = "",
  };
  char err[256] = {0};
  if (ny_native_x86_64_emit_nir(out, &target, f, name, false, err,
                                 sizeof(err)))
    return true;
  fprintf(stderr, "[%s] emit failed: %s\n", name, err);
  return false;
}

int main(void) {
  nyir_func_t boundaries;
  build_boundaries(&boundaries);
  if (!ny_verify(&boundaries, "immediate-remat-boundaries") ||
      !verify_machine(&boundaries, "immediate-remat-boundaries")) {
    nyir_func_free(&boundaries);
    return 1;
  }
  long long result = ny_eval(&boundaries, "immediate-remat-boundaries");
  ny_native_writer_t boundary_asm = {0};
  bool emitted_boundaries = emit_text(&boundaries, "immediate_remat_boundaries",
                                      &boundary_asm);
  nyir_func_free(&boundaries);
  if (!emitted_boundaries)
    return 1;
  bool boundaries_ok = result == 60 &&
      count_text(boundary_asm.data, "\taddq\t$-13, %rax\n") == 1 &&
      count_text(boundary_asm.data, "\taddq\t$-17, %rax\n") == 1 &&
      count_text(boundary_asm.data, "\taddq\t%r10, %rax\n") >= 2;
  free(boundary_asm.data);
  if (!boundaries_ok) {
    fprintf(stderr, "[immediate-remat-boundaries] immediate/fallback metric failed\n");
    return 1;
  }

  nyir_func_t call;
  build_call(&call);
  if (!ny_verify(&call, "immediate-remat-call") ||
      !verify_machine(&call, "immediate-remat-call")) {
    nyir_func_free(&call);
    return 1;
  }
  ny_native_writer_t call_asm = {0};
  bool emitted_call = emit_text(&call, "immediate_remat_call", &call_asm);
  nyir_func_free(&call);
  if (!emitted_call)
    return 1;
  const char *after_call = strstr(call_asm.data, "\tcall\tny_fn_touch\n");
  bool call_ok = count_text(call_asm.data, "\taddq\t$-7, %rax\n") == 1 &&
      count_text(call_asm.data, "\taddq\t$-11, %rax\n") == 0 &&
      after_call != NULL &&
      strstr(after_call, "\taddq\t%r10, %rax\n") != NULL;
  free(call_asm.data);
  if (!call_ok) {
    fprintf(stderr, "[immediate-remat-call] immediate/fallback metric failed\n");
    return 1;
  }
  printf("[immediate-remat] safe=3 fallback=3 result=60\n");
  return 0;
}
