/*
 * AVR portable text emission must reject every NYIR operation class it does
 * not implement before writing partial assembly.
 */
#include "code/backend/internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool reject_unsupported_op(nyr_op_t op) {
  nyr_inst_t inst = {.op = op};
  nyr_func_t ir = {.data = &inst, .len = 1};
  static const ny_native_target_info_t target = {
      .target = NY_NATIVE_TARGET_AVR,
      .abi = NY_NATIVE_ABI_SYSV,
      .target_name = "avr",
      .object_format = "elf",
      .symbol_prefix = "",
  };
  nyr_writer_t assembly = {0};
  char err[256] = {0};
  bool emitted = nyr_avr_emit_ir(&assembly, &target, &ir, "unsupported", false,
                                 err, sizeof(err));
  bool rejected = !emitted && !assembly.data && assembly.len == 0 &&
                  strstr(err, "AVR portable emitter does not support NYIR "
                              "operation") != NULL;
  if (!rejected)
    fprintf(stderr, "[portable-avr] %s was not rejected cleanly: %s\n",
            nyr_op_name(op), err[0] ? err : "unexpected emitted assembly");
  free(assembly.data);
  return rejected;
}

int main(void) {
  static const nyr_op_t unsupported[] = {
      NYIR_SQRT_F64,
      NYIR_SIN_F64,
      NYIR_COS_F64,
      NYIR_VEC4_LOAD_F64,
      NYIR_VEC4_STORE_F64,
      NYIR_VEC4_ADD_F64,
      NYIR_VEC4_SUB_F64,
      NYIR_VEC4_MUL_F64,
      NYIR_VEC4_DIV_F64,
      NYIR_VEC4_FMA_F64,
      NYIR_VEC4_SET1_F64,
      NYIR_VEC4_SHUFFLE_F64,
      NYIR_VEC8_LOAD_F32,
      NYIR_VEC8_STORE_F32,
      NYIR_VEC8_ADD_F32,
      NYIR_VEC8_SUB_F32,
      NYIR_VEC8_MUL_F32,
      NYIR_VEC8_DIV_F32,
      NYIR_VEC8_FMA_F32,
      NYIR_VEC8_SET1_F32,
      NYIR_VEC8_SHUFFLE_F32,
      NYIR_VEC4_LOAD_I64,
      NYIR_VEC4_STORE_I64,
      NYIR_VEC4_ADD_I64,
      NYIR_VEC4_SUB_I64,
      NYIR_VEC4_AND_I64,
      NYIR_VEC4_OR_I64,
      NYIR_VEC4_XOR_I64,
      NYIR_VEC4_SHL_I64,
      NYIR_VEC4_SAR_I64,
      NYIR_VEC4_REDUCE_ADD_I64,
      NYIR_VEC8_LOAD_I64,
      NYIR_VEC8_STORE_I64,
      NYIR_VEC8_ADD_I64,
      NYIR_VEC8_SUB_I64,
      NYIR_VEC8_AND_I64,
      NYIR_VEC8_OR_I64,
      NYIR_VEC8_XOR_I64,
      NYIR_VEC8_REDUCE_ADD_I64,
  };
  for (size_t i = 0; i < sizeof(unsupported) / sizeof(unsupported[0]); ++i)
    if (!reject_unsupported_op(unsupported[i]))
      return 1;
  puts("portable AVR unsupported-operation matrix passed");
  return 0;
}
