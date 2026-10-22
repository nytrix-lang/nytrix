/*
 * Literal immediates remain usable across fixed-register clobbers; dynamic
 * divisors and shift counts must still use the register-based fallback.
 */
#include "code/backend/internal.h"
#include "code/backend/ir/machine.h"
#include "support.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool emit_text(const nyr_func_t *f, const char *name,
                      nyr_writer_t *out);

static int ny_copy(nyr_func_t *f, int source) {
  return nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_COPY,
                                    .dst = -1,
                                    .a = source,
                                    .b = -1,
                                    .c = -1,
                                    .d = -1,
                                    .e = -1,
                                    .f = -1});
}

static void ny_store_local(nyr_func_t *f, int value, int slot) {
  nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_STORE_LOCAL,
                             .dst = -1,
                             .a = value,
                             .b = -1,
                             .c = -1,
                             .d = -1,
                             .e = -1,
                             .f = -1,
                             .imm = slot});
}

static int ny_load_local(nyr_func_t *f, int slot) {
  return nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_LOAD_LOCAL,
                                    .dst = -1,
                                    .a = -1,
                                    .b = -1,
                                    .c = -1,
                                    .d = -1,
                                    .e = -1,
                                    .f = -1,
                                    .imm = slot});
}

static void build_boundaries(nyr_func_t *f) {
  nyr_opt_begin(f);
  int base = nyr_opt_const(f, 40);
  int div_imm = nyr_opt_const(f, -13);
  int safe_div = nyr_opt_binop(f, NYIR_ADD_I64, base, div_imm);
  int divisor = nyr_opt_const(f, 3);
  ny_store_local(f, divisor, 0);
  int variable_divisor = ny_load_local(f, 0);
  int dividend = nyr_opt_const(f, 30);
  int quotient = nyr_opt_binop(f, NYIR_DIV_I64, dividend, variable_divisor);
  int after_div = nyr_opt_binop(f, NYIR_ADD_I64, quotient, div_imm);
  int shift_imm = nyr_opt_const(f, -17);
  int safe_shift = nyr_opt_binop(f, NYIR_ADD_I64, base, shift_imm);
  int variable_count = ny_load_local(f, 0);
  int shifted = nyr_opt_binop(f, NYIR_SHL_I64, quotient, variable_count);
  int constant_count = nyr_opt_const(f, 1);
  int constant_shifted =
      nyr_opt_binop(f, NYIR_SHL_I64, quotient, constant_count);
  int after_shift = nyr_opt_binop(f, NYIR_ADD_I64, shifted, shift_imm);
  int combined = nyr_opt_binop(f, NYIR_ADD_I64, after_div, after_shift);
  (void)safe_div;
  (void)safe_shift;
  (void)constant_shifted;
  nyr_opt_ret(f, combined);
}

static void build_call(nyr_func_t *f) {
  nyr_opt_begin(f);
  int base = nyr_opt_const(f, 40);
  int safe_imm = nyr_opt_const(f, -7);
  int safe_copy = ny_copy(f, safe_imm);
  int safe = nyr_opt_binop(f, NYIR_ADD_I64, base, safe_copy);
  int after_call_imm = nyr_opt_const(f, -11);
  int after_call_copy = ny_copy(f, after_call_imm);
  nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_CALL,
                             .dst = -1,
                             .a = -1,
                             .b = -1,
                             .c = -1,
                             .d = -1,
                             .e = -1,
                             .f = -1,
                             .imm = 0,
                             .symbol = "touch"});
  int after_call = nyr_opt_binop(f, NYIR_ADD_I64, base, after_call_copy);
  (void)safe;
  nyr_opt_ret(f, after_call);
}

static void build_uniform_multidef(nyr_func_t *f, int *duplicate_def,
                                   int *shared_value) {
  nyr_opt_begin(f);
  int lhs = nyr_opt_const(f, 6);
  int rhs = nyr_opt_const(f, 7);
  int same_rhs = nyr_opt_const(f, 7);
  *duplicate_def = same_rhs;
  *shared_value = rhs;
  int product = nyr_opt_binop(f, NYIR_MUL_I64, lhs, rhs);
  nyr_opt_ret(f, product);
}

static void build_machine_mul(nyr_func_t *f) {
  nyr_opt_begin(f);
  f->param_types = malloc(sizeof(*f->param_types));
  if (!f->param_types)
    exit(2);
  f->param_types[0] = NYIR_PARAM_I64;
  f->param_count = 1;
  int value = ny_load_local(f, 0);
  value = nyr_opt_binop(f, NYIR_ADD_I64, value, nyr_opt_const(f, 7));
  value = nyr_opt_binop(f, NYIR_SUB_I64, value, nyr_opt_const(f, -3));
  value = nyr_opt_binop(f, NYIR_AND_I64, value, nyr_opt_const(f, 32767));
  value = nyr_opt_binop(f, NYIR_OR_I64, value, nyr_opt_const(f, 1));
  value = nyr_opt_binop(f, NYIR_XOR_I64, value, nyr_opt_const(f, 3));
  int scale = nyr_opt_const(f, 3877);
  value = nyr_opt_binop(f, NYIR_MUL_I64, value, scale);
  value = nyr_opt_binop(f, NYIR_SHL_I64, value, nyr_opt_const(f, 3));
  value = nyr_opt_binop(f, NYIR_ADD_I64, value,
                        nyr_opt_const(f, INT64_C(4294967296)));
  nyr_opt_ret(f, value);
}

static bool check_nir_stack_home_coloring(void) {
  nyr_func_t f;
  nyr_opt_begin(&f);
  f.param_count = 1;
  f.param_types = malloc(sizeof(*f.param_types));
  if (!f.param_types)
    return false;
  f.param_types[0] = NYIR_PARAM_I64;
  for (int i = 1; i <= 12; ++i) {
    int value = ny_load_local(&f, 0);
    int addend = nyr_opt_const(&f, i);
    value = nyr_opt_binop(&f, NYIR_ADD_I64, value, addend);
    ny_store_local(&f, value, 0);
  }
  nyr_opt_ret(&f, ny_load_local(&f, 0));
  bool ok = nyr_opt_verify(&f, "x64-nir-stack-home-coloring");
  int64_t interpreted = ok ? nyr_opt_eval(&f, "x64-nir-stack-home-coloring")
                           : 0;
  nyr_writer_t assembly = {0};
  ok = ok && emit_text(&f, "x64_nir_stack_home_coloring", &assembly);
  size_t frame = SIZE_MAX;
  if (ok && assembly.data) {
    const char *sub = strstr(assembly.data, "\tsubq\t$");
    if (sub) {
      char *end = NULL;
      unsigned long parsed = strtoul(sub + strlen("\tsubq\t$"), &end, 10);
      if (end && strncmp(end, ", %rsp", 6) == 0)
        frame = (size_t)parsed;
    }
  }
  /*
   * Twelve disjoint temporary chains should fit in three 32-byte homes.
   */
  ok = ok && interpreted == 78 && frame <= 128;
  if (!ok)
    fprintf(stderr,
            "[x64-nir-stack-home-coloring] expected result=78 and frame<=128 "
            "(result=%lld frame=%zu)\n%s",
            (long long)interpreted, frame,
            assembly.data ? assembly.data : "<no assembly>\n");
  free(assembly.data);
  nyr_ir_func_free(&f);
  return ok;
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

static size_t count_stack_adds(const char *text) {
  size_t count = 0;
  while (text && *text) {
    const char *end = strchr(text, '\n');
    size_t len = end ? (size_t)(end - text) : strlen(text);
    const char *stack_add = strstr(text, "(%rsp), %rax");
    if (len >= 5 && strstr(text, "\taddq\t") == text && stack_add &&
        stack_add < text + len)
      ++count;
    if (!end)
      break;
    text = end + 1;
  }
  return count;
}

static bool verify_machine(const nyr_func_t *f, const char *name) {
  nyr_mach_func_t mach = {0};
  char err[256] = {0};
  bool ok = nyr_mach_lower_nir(f, &mach, 0, err, sizeof(err)) &&
            nyr_mach_verify(&mach, 0, err, sizeof(err));
  if (!ok)
    fprintf(stderr, "[%s] machine verification failed: %s\n", name, err);
  nyr_mach_func_free(&mach);
  return ok;
}

static bool emit_text(const nyr_func_t *f, const char *name,
                      nyr_writer_t *out) {
  static const ny_native_target_info_t target = {
      .target = NY_NATIVE_TARGET_X86_64,
      .abi = NY_NATIVE_ABI_SYSV,
      .object_format = "elf",
      .symbol_prefix = "",
  };
  char err[256] = {0};
  if (nyr_x86_64_emit_ir(out, &target, f, name, false, err, sizeof(err)))
    return true;
  fprintf(stderr, "[%s] emit failed: %s\n", name, err);
  return false;
}

int main(void) {
  if (!check_nir_stack_home_coloring())
    return 1;
  nyr_func_t boundaries;
  build_boundaries(&boundaries);
  if (!nyr_opt_verify(&boundaries, "immediate-remat-boundaries") ||
      !verify_machine(&boundaries, "immediate-remat-boundaries")) {
    nyr_ir_func_free(&boundaries);
    return 1;
  }
  long long result = nyr_opt_eval(&boundaries, "immediate-remat-boundaries");
  nyr_writer_t boundary_asm = {0};
  bool emitted_boundaries =
      emit_text(&boundaries, "immediate_remat_boundaries", &boundary_asm);
  nyr_ir_func_free(&boundaries);
  if (!emitted_boundaries)
    return 1;
  /*
   * The final `combined` add consumes two non-rematerializable stack slots.
   * The emitter lowers that shape as a direct memory operand instead of the
   * historical push/pop %r10 sequence, so the fallback assertion tracks the
   * memory form rather than requiring the r10 scratch copy.
   */
  bool boundaries_ok =
      result == 60 &&
      count_text(boundary_asm.data, "\taddq\t$-13, %rax\n") == 2 &&
      count_text(boundary_asm.data, "\taddq\t$-17, %rax\n") == 2 &&
      count_text(boundary_asm.data, "\tshlq\t$1, %rax\n") == 1 &&
      count_stack_adds(boundary_asm.data) == 1;
  if (!boundaries_ok) {
    fprintf(stderr,
            "[immediate-remat-boundaries] literal/fallback metric failed "
            "(result=%lld, -13=%zu, -17=%zu, shift-immediate=%zu, "
            "memory-fallback=%zu)\n%s",
            result, count_text(boundary_asm.data, "\taddq\t$-13, %rax\n"),
            count_text(boundary_asm.data, "\taddq\t$-17, %rax\n"),
            count_text(boundary_asm.data, "\tshlq\t$1, %rax\n"),
            count_stack_adds(boundary_asm.data),
            boundary_asm.data ? boundary_asm.data : "<no assembly>\n");
    free(boundary_asm.data);
    return 1;
  }
  free(boundary_asm.data);

  nyr_func_t call;
  build_call(&call);
  if (!nyr_opt_verify(&call, "immediate-remat-call") ||
      !verify_machine(&call, "immediate-remat-call")) {
    nyr_ir_func_free(&call);
    return 1;
  }
  nyr_writer_t call_asm = {0};
  bool emitted_call = emit_text(&call, "immediate_remat_call", &call_asm);
  nyr_ir_func_free(&call);
  if (!emitted_call)
    return 1;
  const char *after_call = strstr(call_asm.data, "\tcall\tny_fn_touch\n");
  bool call_ok = count_text(call_asm.data, "\taddq\t$-7, %rax\n") == 1 &&
                 count_text(call_asm.data, "\taddq\t$-11, %rax\n") == 1 &&
                 after_call != NULL &&
                 strstr(after_call, "\taddq\t$-11, %rax\n") != NULL;
  free(call_asm.data);
  if (!call_ok) {
    fprintf(stderr,
            "[immediate-remat-call] immediate/fallback metric failed\n");
    return 1;
  }

  /*
   * The direct native emitter also accepts post-PHI-lowering NYIR, where
   * several edge copies can define one value.  Identical literal definitions
   * are still an immediate; a mixed-value definition must remain a home.
   */
  nyr_func_t multidef;
  int duplicate_def = -1;
  int shared_value = -1;
  build_uniform_multidef(&multidef, &duplicate_def, &shared_value);
  if (!nyr_opt_verify(&multidef, "immediate-remat-uniform-multidef")) {
    nyr_ir_func_free(&multidef);
    return 1;
  }
  long long multidef_result =
      nyr_opt_eval(&multidef, "immediate-remat-uniform-multidef");
  for (size_t i = 0; i < multidef.len; ++i) {
    if (multidef.data[i].dst == duplicate_def &&
        multidef.data[i].op == NYIR_CONST_I64) {
      multidef.data[i].dst = shared_value;
    }
  }
  nyr_writer_t multidef_asm = {0};
  bool emitted_multidef =
      emit_text(&multidef, "immediate_remat_uniform_multidef", &multidef_asm);
  nyr_ir_func_free(&multidef);
  if (!emitted_multidef || multidef_result != 42 ||
      count_text(multidef_asm.data, "\timulq\t$7, %rax, %rax\n") != 1) {
    fprintf(stderr,
            "[immediate-remat-multidef] expected identical edge literals to "
            "select an immediate (result=%lld)\n%s",
            multidef_result,
            multidef_asm.data ? multidef_asm.data : "<no assembly>\n");
    free(multidef_asm.data);
    return 1;
  }
  free(multidef_asm.data);

  /*
   * The normal x86 route emits scalar machine IR, not the legacy NIR text
   * emitter above.  Exercise that production selector directly.
   */
  nyr_func_t machine_ir;
  build_machine_mul(&machine_ir);
  int64_t locals[] = {1000};
  nyr_eval_result_t eval = {0};
  char eval_err[256] = {0};
  bool eval_ok = nyr_opt_verify(&machine_ir, "immediate-machine-mul") &&
                 nyr_ir_eval(&machine_ir, locals, 1, 500, &eval, eval_err,
                           sizeof(eval_err)) &&
                 eval.returned && eval.result == INT64_C(4326231424);
  if (!eval_ok) {
    fprintf(stderr, "[immediate-machine-mul] evaluator failed: %s\n", eval_err);
    nyr_eval_result_free(&eval);
    nyr_ir_func_free(&machine_ir);
    return 1;
  }
  nyr_eval_result_free(&eval);
  nyr_mach_func_t machine = {0};
  char machine_err[256] = {0};
  bool lowered = nyr_mach_lower_nir(&machine_ir, &machine, 0, machine_err,
                                   sizeof(machine_err)) &&
                 nyr_mach_verify(&machine, 0, machine_err, sizeof(machine_err));
  nyr_ir_func_free(&machine_ir);
  if (!lowered) {
    fprintf(stderr, "[immediate-machine-mul] lowering failed: %s\n",
            machine_err);
    nyr_mach_func_free(&machine);
    return 1;
  }
  static const ny_native_target_info_t target = {
      .target = NY_NATIVE_TARGET_X86_64,
      .abi = NY_NATIVE_ABI_SYSV,
      .object_format = "elf",
      .symbol_prefix = "",
  };
  nyr_writer_t machine_asm = {0};
  bool machine_emitted = nyr_x86_64_emit_mach_scalar(
      &machine_asm, &target, &machine, "immediate_machine_mul", false,
      machine_err, sizeof(machine_err));
  nyr_mach_func_free(&machine);
  if (!machine_emitted ||
      count_text(machine_asm.data, "\taddq\t$7, %rax\n") != 1 ||
      count_text(machine_asm.data, "\tsubq\t$-3, %rax\n") != 1 ||
      count_text(machine_asm.data, "\tandq\t$32767, %rax\n") != 1 ||
      count_text(machine_asm.data, "\torq\t$1, %rax\n") != 1 ||
      count_text(machine_asm.data, "\txorq\t$3, %rax\n") != 1 ||
      count_text(machine_asm.data, "\timulq\t$3877, %rax\n") != 1 ||
      count_text(machine_asm.data, "\tshlq\t$3, %rax\n") != 1 ||
      count_text(machine_asm.data, "\taddq\t-") != 1 ||
      count_text(machine_asm.data, "\timulq\t-") != 0) {
    fprintf(stderr,
            "[immediate-machine-mul] constant multiply was not "
            "selected as an immediate: %s\n%s",
            machine_err,
            machine_asm.data ? machine_asm.data : "<no assembly>\n");
    free(machine_asm.data);
    return 1;
  }
  free(machine_asm.data);
  printf("[immediate-remat] literal-across-clobbers=6 dynamic-fallback=1 "
         "uniform-multidef-immediate=1 machine-alu-immediates=6 "
         "machine-shift-immediate=1 wide-fallback=1 "
         "result=60/42/4326231424\n");
  return 0;
}
