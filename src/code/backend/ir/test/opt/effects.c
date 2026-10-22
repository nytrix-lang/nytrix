/*
 * Exhaustive contract test for the opcode-level NYIR effect table.
 */
#include <limits.h>
#include <nyr/ir.h>
#include <stdio.h>

static unsigned expected_effects(nyr_op_t op) {
  switch (op) {
  case NYIR_NOP:
  case NYIR_CONST_I64:
  case NYIR_COPY:
  case NYIR_PHI:
  case NYIR_ADD_I64:
  case NYIR_SUB_I64:
  case NYIR_MUL_I64:
  case NYIR_AND_I64:
  case NYIR_OR_I64:
  case NYIR_XOR_I64:
  case NYIR_SHL_I64:
  case NYIR_SAR_I64:
  case NYIR_ROR_I64:
  case NYIR_CMP_I64:
  case NYIR_LABEL:
  case NYIR_CONST_F64:
  case NYIR_ADD_F64:
  case NYIR_SUB_F64:
  case NYIR_MUL_F64:
  case NYIR_DIV_F64:
  case NYIR_I64_TO_F64:
  case NYIR_F64_TO_I64:
  case NYIR_F32_TO_I64:
  case NYIR_CMP_F64:
  case NYIR_SQRT_F64:
  case NYIR_CONST_F32:
  case NYIR_ADD_F32:
  case NYIR_SUB_F32:
  case NYIR_MUL_F32:
  case NYIR_DIV_F32:
  case NYIR_I64_TO_F32:
  case NYIR_F64_TO_F32:
  case NYIR_F32_TO_F64:
  case NYIR_CMP_F32:
  case NYIR_ADDR_LOCAL:
  case NYIR_ADDR_SYMBOL:
  case NYIR_CAPTURE_RET:
  case NYIR_VEC4_ADD_F64:
  case NYIR_VEC4_SUB_F64:
  case NYIR_VEC4_MUL_F64:
  case NYIR_VEC4_DIV_F64:
  case NYIR_VEC4_FMA_F64:
  case NYIR_VEC4_SET1_F64:
  case NYIR_VEC4_SHUFFLE_F64:
  case NYIR_VEC8_ADD_F32:
  case NYIR_VEC8_SUB_F32:
  case NYIR_VEC8_MUL_F32:
  case NYIR_VEC8_DIV_F32:
  case NYIR_VEC8_FMA_F32:
  case NYIR_VEC8_SET1_F32:
  case NYIR_VEC8_SHUFFLE_F32:
  case NYIR_VEC4_ADD_I64:
  case NYIR_VEC4_SUB_I64:
  case NYIR_VEC4_AND_I64:
  case NYIR_VEC4_OR_I64:
  case NYIR_VEC4_XOR_I64:
  case NYIR_VEC4_SHL_I64:
  case NYIR_VEC4_SAR_I64:
  case NYIR_VEC4_REDUCE_ADD_I64:
  case NYIR_VEC8_ADD_I64:
  case NYIR_VEC8_SUB_I64:
  case NYIR_VEC8_AND_I64:
  case NYIR_VEC8_OR_I64:
  case NYIR_VEC8_XOR_I64:
  case NYIR_VEC8_REDUCE_ADD_I64:
  case NYIR_SIN_F64:
  case NYIR_COS_F64:
  case NYIR_VEC4_REDUCE_ADD_F64:
  case NYIR_VEC4_SET1_I64:
  case NYIR_SELECT_I64:
  case NYIR_ROR32_I64:
  case NYIR_BSWAP_I64:
  case NYIR_BSWAP32_I64:
    return NYIR_EFFECT_NONE;
  case NYIR_LOAD_LOCAL:
    return NYIR_EFFECT_READ_LOCAL;
  case NYIR_STORE_LOCAL:
    return NYIR_EFFECT_WRITE_LOCAL;
  case NYIR_LOAD_I64:
  case NYIR_VEC4_LOAD_F64:
  case NYIR_VEC8_LOAD_F32:
  case NYIR_VEC4_LOAD_I64:
  case NYIR_VEC8_LOAD_I64:
    return NYIR_EFFECT_READ_MEMORY | NYIR_EFFECT_MAY_TRAP;
  case NYIR_STORE_I64:
  case NYIR_VEC4_STORE_F64:
  case NYIR_VEC8_STORE_F32:
  case NYIR_VEC4_STORE_I64:
  case NYIR_VEC8_STORE_I64:
    return NYIR_EFFECT_WRITE_MEMORY | NYIR_EFFECT_MAY_TRAP;
  case NYIR_DIV_I64:
  case NYIR_MOD_I64:
    return NYIR_EFFECT_MAY_TRAP;
  case NYIR_BOUNDS_CHECK:
    return NYIR_EFFECT_CONTROL | NYIR_EFFECT_MAY_TRAP;
  case NYIR_COPY_STRUCT:
    return NYIR_EFFECT_READ_MEMORY | NYIR_EFFECT_WRITE_MEMORY |
           NYIR_EFFECT_MAY_TRAP;
  case NYIR_ALLOCA:
    return NYIR_EFFECT_ALLOCATION;
  case NYIR_CALL:
    return NYIR_EFFECT_CALL | NYIR_EFFECT_UNKNOWN_SIDE_EFFECT;
  case NYIR_RET:
  case NYIR_BR:
  case NYIR_BR_IF:
    return NYIR_EFFECT_CONTROL;
  case NYIR_OP_COUNT:
    return NYIR_EFFECT_UNKNOWN_SIDE_EFFECT;
  }
  /*
   * A new opcode must be classified explicitly in both tables.
   */
  return UINT_MAX;
}

int main(void) {
  for (int raw = 0; raw < NYIR_OP_COUNT; ++raw) {
    nyr_op_t op = (nyr_op_t)raw;
    unsigned want = expected_effects(op);
    nyr_inst_t inst = {.op = op};
    unsigned got = nyr_inst_effects(&inst);
    if (want == UINT_MAX || got != want) {
      fprintf(stderr, "[effects] %s (%d): expected 0x%x, got 0x%x\n",
              nyr_op_name(op), raw, want, got);
      return 1;
    }
  }
  nyr_inst_t invalid = {.op = (nyr_op_t)NYIR_OP_COUNT};
  nyr_inst_t out_of_range = {.op = (nyr_op_t)(NYIR_OP_COUNT + 7)};
  if (nyr_inst_effects(NULL) != NYIR_EFFECT_NONE ||
      nyr_inst_effects(&invalid) != NYIR_EFFECT_UNKNOWN_SIDE_EFFECT ||
      nyr_inst_effects(&out_of_range) != NYIR_EFFECT_UNKNOWN_SIDE_EFFECT) {
    fprintf(stderr,
            "[effects] null/invalid opcode handling is not conservative\n");
    return 1;
  }
  const unsigned call_effects[] = {
      NYIR_EFFECT_CALL,
      NYIR_EFFECT_CALL | NYIR_EFFECT_READ_MEMORY,
      NYIR_EFFECT_CALL | NYIR_EFFECT_READ_MEMORY | NYIR_EFFECT_ALLOCATION |
          NYIR_EFFECT_MAY_TRAP,
      NYIR_EFFECT_CALL | NYIR_EFFECT_READ_MEMORY | NYIR_EFFECT_MAY_TRAP,
      NYIR_EFFECT_CALL | NYIR_EFFECT_WRITE_MEMORY | NYIR_EFFECT_MAY_TRAP,
      NYIR_EFFECT_CALL | NYIR_EFFECT_READ_MEMORY | NYIR_EFFECT_WRITE_MEMORY |
          NYIR_EFFECT_ALLOCATION,
      NYIR_EFFECT_CALL | NYIR_EFFECT_READ_MEMORY,
      NYIR_EFFECT_CALL | NYIR_EFFECT_READ_MEMORY | NYIR_EFFECT_WRITE_MEMORY |
          NYIR_EFFECT_MAY_TRAP,
      NYIR_EFFECT_CALL | NYIR_EFFECT_READ_MEMORY | NYIR_EFFECT_WRITE_MEMORY |
          NYIR_EFFECT_MAY_TRAP,
      NYIR_EFFECT_CALL | NYIR_EFFECT_READ_MEMORY | NYIR_EFFECT_WRITE_MEMORY |
          NYIR_EFFECT_MAY_TRAP,
      NYIR_EFFECT_CALL | NYIR_EFFECT_READ_MEMORY | NYIR_EFFECT_ALLOCATION |
          NYIR_EFFECT_MAY_TRAP,
      NYIR_EFFECT_CALL | NYIR_EFFECT_VOLATILE | NYIR_EFFECT_IO,
      NYIR_EFFECT_CALL | NYIR_EFFECT_FFI | NYIR_EFFECT_UNKNOWN_SIDE_EFFECT,
  };
  const char *call_names[] = {"rt_value_to_f64",
                              "rt_native_is_int",
                              "rt_cstr_to_str",
                              "rt_load8_idx",
                              "rt_store8_idx",
                              "rt_retain_owned",
                              "rt_rc_count",
                              "rt_release_owned",
                              "rt_drop_owned",
                              "rt_drop_owned_slot",
                              "rt_len_strict",
                              "rt_monotonic_ns_raw",
                              NULL};
  for (size_t i = 0; i < sizeof(call_effects) / sizeof(call_effects[0]); ++i) {
    nyr_inst_t call = {.op = NYIR_CALL, .symbol = call_names[i]};
    unsigned got = nyr_call_effect_summary(&call);
    if (got != call_effects[i]) {
      fprintf(stderr, "[effects] call %s: expected 0x%x, got 0x%x\n",
              call_names[i] ? call_names[i] : "<unknown>", call_effects[i],
              got);
      return 1;
    }
  }
  nyr_inst_t contracts = {.op = NYIR_CALL};
  for (size_t arg = 0; arg < NYIR_CALL_MAX_ARGS; ++arg)
    nyr_call_arg_contract_set(&contracts, arg, (uint8_t)(arg % 4));
  for (size_t arg = 0; arg < NYIR_CALL_MAX_ARGS; ++arg) {
    uint8_t want = (uint8_t)(arg % 4);
    uint8_t got = nyr_call_arg_contract_get(&contracts, arg);
    if (got != want) {
      fprintf(stderr, "[effects] call contract slot %zu: expected %u, got %u\n",
              arg, (unsigned)want, (unsigned)got);
      return 1;
    }
  }
  return 0;
}
