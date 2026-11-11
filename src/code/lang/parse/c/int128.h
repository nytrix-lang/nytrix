/*
 * Internal helpers for lowering 128-bit aggregate values.
 */
static int lower_int128_word(c_parser_t *p, int base, size_t offset) {
  int address = lower_add_const(p, base, offset);
  if (address < 0)
    return -1;
  return emit_value(
      p, (nyr_inst_t){.op = NYIR_LOAD_I64,
                      .dst = -1,
                      .a = address,
                      .effects = NYIR_EFFECT_READ_MEMORY | NYIR_EFFECT_MAY_TRAP,
                      .scalar_class = NYIR_SCALAR_I64_RAW});
}

static int lower_int128_ushr(c_parser_t *p, int value, int count) {
  return emit_value(p, (nyr_inst_t){.op = NYIR_INLINE_ASM,
                                    .dst = -1,
                                    .a = value,
                                    .b = count,
                                    .imm = -23,
                                    .effects = NYIR_EFFECT_NONE,
                                    .symbol = "unsigned-shift-right"});
}

static int lower_int128_store(c_parser_t *p, int address, size_t offset,
                              int value) {
  int target = lower_add_const(p, address, offset);
  if (target < 0)
    return -1;
  return emit(p,
              (nyr_inst_t){.op = NYIR_STORE_I64,
                           .dst = -1,
                           .a = target,
                           .c = value,
                           .effects =
                               NYIR_EFFECT_WRITE_MEMORY | NYIR_EFFECT_MAY_TRAP,
                           .scalar_class = NYIR_SCALAR_I64_RAW},
              NULL)
             ? target
             : -1;
}

static bool lower_int128_store_integer(c_parser_t *p, int address,
                                       c_value_t value) {
  if (address < 0 || value.value < 0 || !c_type_is_integer(value.type))
    return false;
  bool signed_value = value.type == C_I16 || value.type == C_I32 ||
                      value.type == C_I64 ||
                      (value.type == C_CHAR && !p->unsigned_char);
  int low = normalize_integer(p, signed_value ? C_I64 : C_U64, value.value);
  int high = emit_i64_constant(p, 0);
  if (low < 0 || high < 0)
    return false;
  if (signed_value) {
    int shift = emit_i64_constant(p, 63);
    high =
        shift < 0
            ? -1
            : emit_value(p, (nyr_inst_t){.op = NYIR_SAR_I64,
                                         .dst = -1,
                                         .a = low,
                                         .b = shift,
                                         .scalar_class = NYIR_SCALAR_I64_RAW});
    if (high < 0)
      return false;
  }
  return lower_int128_store(p, address, 0, low) >= 0 &&
         lower_int128_store(p, address, 8, high) >= 0;
}

static bool lower_int128_compound_bitwise(c_parser_t *p, int address,
                                          const c_agg_type_t *agg,
                                          c_value_t value, nyr_op_t op) {
  if (address < 0 || !agg || !agg->is_int128 || value.value < 0)
    return false;
  int right_low;
  int right_high;
  if (value.type == C_AGG && value.agg && value.agg->is_int128) {
    right_low = lower_int128_word(p, value.value, 0);
    right_high = lower_int128_word(p, value.value, 8);
  } else {
    if (!c_type_is_integer(value.type))
      return false;
    bool signed_value = value.type == C_I16 || value.type == C_I32 ||
                        value.type == C_I64 ||
                        (value.type == C_CHAR && !p->unsigned_char);
    right_low = normalize_integer(p, signed_value ? C_I64 : C_U64, value.value);
    right_high = emit_i64_constant(p, 0);
    if (signed_value) {
      int shift = emit_i64_constant(p, 63);
      right_high = shift < 0
                       ? -1
                       : emit_value(p, (nyr_inst_t){.op = NYIR_SAR_I64,
                                                    .dst = -1,
                                                    .a = right_low,
                                                    .b = shift,
                                                    .scalar_class =
                                                        NYIR_SCALAR_I64_RAW});
    }
    if (right_low < 0 || right_high < 0)
      return false;
  }
  int left_low = lower_int128_word(p, address, 0);
  int left_high = lower_int128_word(p, address, 8);
  int result_low =
      left_low < 0 || right_low < 0
          ? -1
          : emit_value(p, (nyr_inst_t){.op = op,
                                       .dst = -1,
                                       .a = left_low,
                                       .b = right_low,
                                       .scalar_class = NYIR_SCALAR_I64_RAW});
  int result_high =
      left_high < 0 || right_high < 0
          ? -1
          : emit_value(p, (nyr_inst_t){.op = op,
                                       .dst = -1,
                                       .a = left_high,
                                       .b = right_high,
                                       .scalar_class = NYIR_SCALAR_I64_RAW});
  return result_low >= 0 && result_high >= 0 &&
         lower_int128_store(p, address, 0, result_low) >= 0 &&
         lower_int128_store(p, address, 8, result_high) >= 0;
}

static int lower_int128_allocate(c_parser_t *p, const c_agg_type_t *agg,
                                 size_t *offset) {
  if (!agg || !agg->is_int128 || !agg_area_alloc(p, agg->size, offset))
    return -1;
  return lower_add_const(p, p->agg_base, *offset);
}
