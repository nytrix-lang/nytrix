static char *ny_native_asm_trim(char *s) {
  while (*s && isspace((unsigned char)*s))
  s++;
  char *end = s + strlen(s);
  while (end > s && isspace((unsigned char)end[-1]))
  *--end = '\0';
  return s;
}

static size_t ny_native_asm_split(char *s, char delimiter, char **parts,
                                size_t cap) {
  size_t count = 0;
  int square = 0, round = 0, curly = 0;
  if (cap)
  parts[count++] = s;
  for (char *p = s; *p; ++p) {
  if (*p == '[') square++;
  else if (*p == ']') square--;
  else if (*p == '(') round++;
  else if (*p == ')') round--;
  else if (*p == '{') curly++;
  else if (*p == '}') curly--;
  else if (*p == delimiter && square == 0 && round == 0 && curly == 0) {
    *p = '\0';
    if (count < cap)
      parts[count++] = p + 1;
  }
  }
  return count;
}

static bool ny_native_asm_constraint_class(const char *s, bool *memory,
                                           bool *immediate, unsigned *bits,
                                           char fixed[16], int *match) {
  *memory = false;
  *immediate = false;
  *bits = 64;
  *match = -1;
  fixed[0] = '\0';
  while (*s == '=' || *s == '+' || *s == '&' || *s == '%' || *s == '*' ||
         *s == '?' || *s == '!')
    s++;
  if (*s == '{') {
    const char *end = strchr(s + 1, '}');
    if (!end || end == s + 1 || (size_t)(end - s - 1) >= 16)
      return false;
    memcpy(fixed, s + 1, (size_t)(end - s - 1));
    fixed[end - s - 1] = '\0';
    for (char *p = fixed; *p; ++p)
      *p = (char)tolower((unsigned char)*p);
    if (fixed[0] == 'w')
      *bits = 32;
    return true;
  }
  if (isdigit((unsigned char)*s)) {
    char *end = NULL;
    long value = strtol(s, &end, 10);
    if (end == s || value < 0 || value >= NY_NATIVE_ASM_MAX_OPERANDS)
      return false;
    *match = (int)value;
    return true;
  }
  if (!*s)
    return false;
  if (*s == 'm' || *s == 'Q' || *s == 'U')
    *memory = true;
  else if (*s == 'i' || *s == 'n' || (*s >= 'I' && *s <= 'P') || *s == 'S')
    *immediate = true;
  if (*s == 'w')
    *bits = 32;
  return true;
}

static bool ny_native_asm_parse_constraints(ny_native_nir_builder_t *b,
                                             const expr_t *e,
                                             ny_native_asm_state_t *state) {
  char buf[2048];
  const char *constraints = e->as.as_asm.constraints ? e->as.as_asm.constraints : "";
  size_t len = strlen(constraints);
  if (len >= sizeof(buf))
    return ny_native_nir_fail(b, "native NYIR asm: constraints are too long");
  memcpy(buf, constraints, len + 1);
  char *tokens[NY_NATIVE_ASM_MAX_OPERANDS];
  size_t count = len ? ny_native_asm_split(buf, ',', tokens, NY_NATIVE_ASM_MAX_OPERANDS) : 0;
  if (count >= NY_NATIVE_ASM_MAX_OPERANDS && strchr(tokens[count - 1], ','))
    return ny_native_nir_fail(b, "native NYIR asm: too many operands");
  memset(state, 0, sizeof(*state));
  state->count = count;
  state->result = -1;
  size_t arg = 0;
  for (size_t i = 0; i < count; ++i) {
    char *token = ny_native_asm_trim(tokens[i]);
    char *alt = strchr(token, '|');
    if (alt)
      *alt = '\0';
    ny_native_asm_operand_t *op = &state->operands[i];
    op->match = -1;
    op->value = -1;
    op->clobber = token[0] == '~';
    if (op->clobber)
      continue;
    op->output = strchr(token, '=') != NULL || strchr(token, '+') != NULL;
    op->input = !op->output || strchr(token, '+') != NULL;
    if (!ny_native_asm_constraint_class(token, &op->memory, &op->immediate,
                                        &op->bits, op->fixed, &op->match))
      return ny_native_nir_fail(b, "native NYIR asm: unsupported constraint '%s'", token);
    if (op->match >= 0)
      op->input = true;
    if (op->output && state->result < 0)
      state->result = (int)i;
    if (op->input) {
      if (arg >= e->as.as_asm.args.len)
        return ny_native_nir_fail(b, "native NYIR asm: constraint/input count mismatch");
      int value = ny_native_nir_lower_expr(b, e->as.as_asm.args.data[arg++]);
      if (value < 0)
        return false;
      op->value = value;
      op->initialized = true;
      if (op->match >= 0) {
        if ((size_t)op->match >= count || !state->operands[op->match].output)
          return ny_native_nir_fail(b, "native NYIR asm: invalid matching constraint %d", op->match);
        state->operands[op->match].value = value;
        state->operands[op->match].initialized = true;
      }
    }
  }
  if (arg != e->as.as_asm.args.len)
    return ny_native_nir_fail(b, "native NYIR asm: %zu arguments are not described by constraints",
                              e->as.as_asm.args.len - arg);
  return true;
}

static int ny_native_asm_emit_binop(ny_native_nir_builder_t *b, nyir_op_t op,
                                    int a, int c) {
  int value = nyir_emit(&b->nyir, (nyir_inst_t){.op = op, .dst = -1, .a = a,
                                                    .b = c, .c = -1});
  if (value < 0)
    ny_native_nir_fail(b, NY_NATIVE_ALLOC_FAIL);
  return value;
}

static int ny_native_asm_emit_cmp(ny_native_nir_builder_t *b, nyir_cmp_t cmp,
                                  int a, int c) {
  int value = nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_CMP_I64,
                                                    .dst = -1, .a = a, .b = c,
                                                    .cmp = cmp});
  if (value < 0)
    ny_native_nir_fail(b, NY_NATIVE_ALLOC_FAIL);
  return value;
}

static int ny_native_asm_mask32(ny_native_nir_builder_t *b, int value) {
  int mask = ny_native_nir_emit_const(b, 0xffffffffLL);
  return mask < 0 ? -1 : ny_native_asm_emit_binop(b, NYIR_AND_I64, value, mask);
}

static int ny_native_asm_lsr(ny_native_nir_builder_t *b, int value, int shift) {
  int mask = ny_native_nir_emit_const(b, 63);
  int zero = ny_native_nir_emit_const(b, 0);
  int sixty_four = ny_native_nir_emit_const(b, 64);
  if (mask < 0 || zero < 0 || sixty_four < 0)
    return -1;
  int s = ny_native_asm_emit_binop(b, NYIR_AND_I64, shift, mask);
  int is_zero = ny_native_asm_emit_cmp(b, NYIR_CMP_EQ, s, zero);
  int slot = ny_native_nir_temp_slot(b);
  int zero_label = b->next_label++;
  int end_label = b->next_label++;
  if (s < 0 || is_zero < 0 || !ny_native_nir_emit_br_if(b, is_zero, zero_label))
    return -1;
  int sar = ny_native_asm_emit_binop(b, NYIR_SAR_I64, value, s);
  int sign = ny_native_asm_emit_cmp(b, NYIR_CMP_LT, value, zero);
  int inverse = ny_native_asm_emit_binop(b, NYIR_SUB_I64, sixty_four, s);
  int bias = ny_native_asm_emit_binop(b, NYIR_SHL_I64, sign, inverse);
  int result = ny_native_asm_emit_binop(b, NYIR_ADD_I64, sar, bias);
  if (result < 0 || !ny_native_nir_store_local_value(b, slot, result) ||
      !ny_native_nir_emit_br(b, end_label) ||
      !ny_native_nir_emit_label(b, zero_label) ||
      !ny_native_nir_store_local_value(b, slot, value) ||
      !ny_native_nir_emit_label(b, end_label))
    return -1;
  return ny_native_nir_load_local_value(b, slot);
}

static int ny_native_asm_sdiv(ny_native_nir_builder_t *b, int a, int c) {
  int zero = ny_native_nir_emit_const(b, 0);
  int min = ny_native_nir_emit_const(b, INT64_MIN);
  int neg_one = ny_native_nir_emit_const(b, -1);
  if (zero < 0 || min < 0 || neg_one < 0)
    return -1;
  int div_zero = ny_native_asm_emit_cmp(b, NYIR_CMP_EQ, c, zero);
  int lhs_min = ny_native_asm_emit_cmp(b, NYIR_CMP_EQ, a, min);
  int rhs_neg_one = ny_native_asm_emit_cmp(b, NYIR_CMP_EQ, c, neg_one);
  int overflow = ny_native_asm_emit_binop(b, NYIR_AND_I64, lhs_min, rhs_neg_one);
  int slot = ny_native_nir_temp_slot(b);
  int zero_label = b->next_label++;
  int overflow_label = b->next_label++;
  int end_label = b->next_label++;
  if (div_zero < 0 || overflow < 0 ||
      !ny_native_nir_emit_br_if(b, div_zero, zero_label) ||
      !ny_native_nir_emit_br_if(b, overflow, overflow_label))
    return -1;
  int result = ny_native_asm_emit_binop(b, NYIR_DIV_I64, a, c);
  if (result < 0 || !ny_native_nir_store_local_value(b, slot, result) ||
      !ny_native_nir_emit_br(b, end_label) ||
      !ny_native_nir_emit_label(b, zero_label) ||
      !ny_native_nir_store_local_value(b, slot, zero) ||
      !ny_native_nir_emit_br(b, end_label) ||
      !ny_native_nir_emit_label(b, overflow_label) ||
      !ny_native_nir_store_local_value(b, slot, min) ||
      !ny_native_nir_emit_label(b, end_label))
    return -1;
  return ny_native_nir_load_local_value(b, slot);
}

static int ny_native_asm_uge(ny_native_nir_builder_t *b, int a, int c) {
  int sign = ny_native_nir_emit_const(b, INT64_MIN);
  if (sign < 0)
    return -1;
  int ax = ny_native_asm_emit_binop(b, NYIR_XOR_I64, a, sign);
  int cx = ny_native_asm_emit_binop(b, NYIR_XOR_I64, c, sign);
  return ax < 0 || cx < 0 ? -1 : ny_native_asm_emit_cmp(b, NYIR_CMP_GE, ax, cx);
}

static int ny_native_asm_udiv(ny_native_nir_builder_t *b, int a, int c) {
  int zero = ny_native_nir_emit_const(b, 0);
  int one = ny_native_nir_emit_const(b, 1);
  if (zero < 0 || one < 0)
    return -1;
  int div_zero = ny_native_asm_emit_cmp(b, NYIR_CMP_EQ, c, zero);
  int divisor_high = ny_native_asm_emit_cmp(b, NYIR_CMP_LT, c, zero);
  int dividend_high = ny_native_asm_emit_cmp(b, NYIR_CMP_LT, a, zero);
  int slot = ny_native_nir_temp_slot(b);
  int zero_label = b->next_label++;
  int divisor_high_label = b->next_label++;
  int dividend_high_label = b->next_label++;
  int end_label = b->next_label++;
  if (div_zero < 0 || divisor_high < 0 || dividend_high < 0 ||
      !ny_native_nir_emit_br_if(b, div_zero, zero_label) ||
      !ny_native_nir_emit_br_if(b, divisor_high, divisor_high_label) ||
      !ny_native_nir_emit_br_if(b, dividend_high, dividend_high_label))
    return -1;
  int result = ny_native_asm_emit_binop(b, NYIR_DIV_I64, a, c);
  if (result < 0 || !ny_native_nir_store_local_value(b, slot, result) ||
      !ny_native_nir_emit_br(b, end_label) ||
      !ny_native_nir_emit_label(b, divisor_high_label))
    return -1;
  result = ny_native_asm_uge(b, a, c);
  if (result < 0 || !ny_native_nir_store_local_value(b, slot, result) ||
      !ny_native_nir_emit_br(b, end_label) ||
      !ny_native_nir_emit_label(b, dividend_high_label))
    return -1;
  int half = ny_native_asm_lsr(b, a, one);
  int half_q = half < 0 ? -1 : ny_native_asm_emit_binop(b, NYIR_DIV_I64, half, c);
  int q = half_q < 0 ? -1 : ny_native_asm_emit_binop(b, NYIR_SHL_I64, half_q, one);
  int product = q < 0 ? -1 : ny_native_asm_emit_binop(b, NYIR_MUL_I64, q, c);
  int remainder = product < 0 ? -1 : ny_native_asm_emit_binop(b, NYIR_SUB_I64, a, product);
  int carry = remainder < 0 ? -1 : ny_native_asm_uge(b, remainder, c);
  result = carry < 0 ? -1 : ny_native_asm_emit_binop(b, NYIR_ADD_I64, q, carry);
  if (result < 0 || !ny_native_nir_store_local_value(b, slot, result) ||
      !ny_native_nir_emit_br(b, end_label) ||
      !ny_native_nir_emit_label(b, zero_label) ||
      !ny_native_nir_store_local_value(b, slot, zero) ||
      !ny_native_nir_emit_label(b, end_label))
    return -1;
  return ny_native_nir_load_local_value(b, slot);
}

static bool ny_native_asm_ref(const ny_native_asm_state_t *state, const char *text,
                              int *index, unsigned *bits) {
  char buf[NY_NATIVE_ASM_MAX_TOKEN];
  size_t len = strlen(text);
  if (len >= sizeof(buf))
    return false;
  memcpy(buf, text, len + 1);
  char *s = ny_native_asm_trim(buf);
  *bits = 64;
  if ((s[0] == 'w' || s[0] == 'x') && isdigit((unsigned char)s[1])) {
    char reg[16];
    snprintf(reg, sizeof(reg), "%s", s);
    for (char *p = reg; *p; ++p)
      *p = (char)tolower((unsigned char)*p);
    for (size_t i = 0; i < state->count; ++i)
      if (state->operands[i].fixed[0] && strcmp(state->operands[i].fixed, reg) == 0) {
        *index = (int)i;
        *bits = reg[0] == 'w' ? 32 : 64;
        return true;
      }
  }
  const char *p = strchr(s, '$');
  if (!p)
    p = strchr(s, '%');
  if (!p)
    return false;
  p++;
  if (*p == '{')
    p++;
  if (*p == 'w' || *p == 'x') {
    *bits = *p == 'w' ? 32 : 64;
    p++;
  }
  while (*p && !isdigit((unsigned char)*p))
    p++;
  if (!isdigit((unsigned char)*p))
    return false;
  char *end = NULL;
  long value = strtol(p, &end, 10);
  if (value < 0 || (size_t)value >= state->count)
    return false;
  const char *modifier = strchr(end, ':');
  if (modifier && modifier[1] == 'w')
    *bits = 32;
  *index = (int)value;
  return true;
}

static int ny_native_asm_source(ny_native_nir_builder_t *b,
                                const ny_native_asm_state_t *state,
                                const char *text, unsigned *bits) {
  char buf[NY_NATIVE_ASM_MAX_TOKEN];
  size_t len = strlen(text);
  if (len >= sizeof(buf)) {
    ny_native_nir_fail(b, "native NYIR asm: operand is too long");
    return -1;
  }
  memcpy(buf, text, len + 1);
  char *s = ny_native_asm_trim(buf);
  if (strcasecmp(s, "xzr") == 0 || strcasecmp(s, "wzr") == 0) {
    *bits = tolower((unsigned char)s[0]) == 'w' ? 32 : 64;
    return ny_native_nir_emit_const(b, 0);
  }
  int index = -1;
  if (ny_native_asm_ref(state, s, &index, bits)) {
    const ny_native_asm_operand_t *op = &state->operands[index];
    if (!op->initialized) {
      ny_native_nir_fail(b, "native NYIR asm: operand %d is read before it is written", index);
      return -1;
    }
    int value = op->value;
    if (*bits == 32 || op->bits == 32)
      value = ny_native_asm_mask32(b, value);
    return value;
  }
  if (*s == '#')
    s++;
  errno = 0;
  char *end = NULL;
  long long value = strtoll(s, &end, 0);
  if (!errno && end != s && *ny_native_asm_trim(end) == '\0') {
    *bits = 64;
    return ny_native_nir_emit_const(b, (int64_t)value);
  }
  ny_native_nir_fail(b, "native NYIR asm: unsupported operand '%s'", text);
  return -1;
}

static bool ny_native_asm_assign(ny_native_nir_builder_t *b,
                                 ny_native_asm_state_t *state, const char *text,
                                 int value) {
  int index = -1;
  unsigned bits = 64;
  if (!ny_native_asm_ref(state, text, &index, &bits))
    return ny_native_nir_fail(b, "native NYIR asm: destination '%s' is not constrained", text);
  if (bits == 32 || state->operands[index].bits == 32) {
    value = ny_native_asm_mask32(b, value);
    if (value < 0)
      return false;
  }
  state->operands[index].value = value;
  state->operands[index].initialized = true;
  return true;
}

static int ny_native_asm_address(ny_native_nir_builder_t *b,
                                 const ny_native_asm_state_t *state,
                                 const char *text, unsigned *width) {
  char buf[NY_NATIVE_ASM_MAX_TOKEN * 2];
  size_t len = strlen(text);
  if (len >= sizeof(buf)) {
    ny_native_nir_fail(b, "native NYIR asm: memory operand is too long");
    return -1;
  }
  memcpy(buf, text, len + 1);
  char *s = ny_native_asm_trim(buf);
  if (*s != '[')
    return ny_native_asm_source(b, state, s, width);
  char *end = strrchr(s, ']');
  if (!end)
    return ny_native_nir_fail(b, "native NYIR asm: malformed memory operand '%s'", text), -1;
  *end = '\0';
  char *parts[3];
  size_t count = ny_native_asm_split(s + 1, ',', parts, 3);
  unsigned bits = 64;
  int base = ny_native_asm_source(b, state, parts[0], &bits);
  if (base < 0)
    return -1;
  if (count > 1) {
    int offset = ny_native_asm_source(b, state, parts[1], &bits);
    if (offset < 0)
      return -1;
    base = ny_native_asm_emit_binop(b, NYIR_ADD_I64, base, offset);
  }
  *width = 64;
  return base;
}

static bool ny_native_asm_exec(ny_native_nir_builder_t *b,
                               ny_native_asm_state_t *state, const char *code) {
  char buf[NY_NATIVE_ASM_MAX_TEMPLATE];
  size_t len = strlen(code);
  if (len >= sizeof(buf))
    return ny_native_nir_fail(b, "native NYIR asm: template is too long");
  memcpy(buf, code, len + 1);
  for (char *p = buf; *p; ++p)
    if (*p == ';')
      *p = '\n';
  char *save = NULL;
  for (char *line = strtok_r(buf, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
    char *comment = strstr(line, "//");
    if (comment)
      *comment = '\0';
    line = ny_native_asm_trim(line);
    if (!*line)
      continue;
    char *space = line;
    while (*space && !isspace((unsigned char)*space))
      space++;
    char mnemonic[16];
    size_t mlen = (size_t)(space - line);
    if (mlen == 0 || mlen >= sizeof(mnemonic))
      return ny_native_nir_fail(b, "native NYIR asm: invalid instruction '%s'", line);
    memcpy(mnemonic, line, mlen);
    mnemonic[mlen] = '\0';
    for (char *p = mnemonic; *p; ++p)
      *p = (char)tolower((unsigned char)*p);
    char *operand_text = ny_native_asm_trim(space);
    char *args[4] = {0};
    size_t argc = *operand_text ? ny_native_asm_split(operand_text, ',', args, 4) : 0;
    for (size_t i = 0; i < argc; ++i)
      args[i] = ny_native_asm_trim(args[i]);
    if (strcmp(mnemonic, "nop") == 0 || strcmp(mnemonic, "yield") == 0 ||
        strcmp(mnemonic, "wfe") == 0 || strcmp(mnemonic, "wfi") == 0 ||
        strcmp(mnemonic, "sev") == 0 || strcmp(mnemonic, "isb") == 0 ||
        strcmp(mnemonic, "dmb") == 0 || strcmp(mnemonic, "dsb") == 0)
      continue;
    if (strcmp(mnemonic, "mov") == 0 && argc == 2) {
      unsigned bits = 64;
      int value = ny_native_asm_source(b, state, args[1], &bits);
      if (value < 0 || !ny_native_asm_assign(b, state, args[0], value))
        return false;
      continue;
    }
    if (strcmp(mnemonic, "mvn") == 0 && argc == 2) {
      unsigned bits = 64;
      int value = ny_native_asm_source(b, state, args[1], &bits);
      int all = ny_native_nir_emit_const(b, -1);
      value = value < 0 || all < 0 ? -1 : ny_native_asm_emit_binop(b, NYIR_XOR_I64, value, all);
      if (value < 0 || !ny_native_asm_assign(b, state, args[0], value))
        return false;
      continue;
    }
    if (strcmp(mnemonic, "ldr") == 0 && argc == 2) {
      unsigned width = 64;
      int address = ny_native_asm_address(b, state, args[1], &width);
      if (address < 0 || width != 64)
        return ny_native_nir_fail(b, "native NYIR asm: only 64-bit ldr is supported");
      int value = ny_native_nir_emit_load_i64(b, address);
      if (value < 0 || !ny_native_asm_assign(b, state, args[0], value))
        return false;
      continue;
    }
    if (strcmp(mnemonic, "str") == 0 && argc == 2) {
      unsigned bits = 64, width = 64;
      int value = ny_native_asm_source(b, state, args[0], &bits);
      int address = value < 0 ? -1 : ny_native_asm_address(b, state, args[1], &width);
      if (value < 0 || address < 0 || bits != 64 || width != 64)
        return ny_native_nir_fail(b, "native NYIR asm: only 64-bit str is supported");
      if (!ny_native_nir_emit_store_i64(b, address, value))
        return false;
      continue;
    }
    bool shift = strcmp(mnemonic, "lsl") == 0 || strcmp(mnemonic, "lsr") == 0 ||
                 strcmp(mnemonic, "asr") == 0;
    bool binary = strcmp(mnemonic, "add") == 0 || strcmp(mnemonic, "sub") == 0 ||
                  strcmp(mnemonic, "and") == 0 || strcmp(mnemonic, "eor") == 0 ||
                  strcmp(mnemonic, "orr") == 0 || strcmp(mnemonic, "mul") == 0 ||
                  strcmp(mnemonic, "sdiv") == 0 || strcmp(mnemonic, "udiv") == 0 ||
                  strcmp(mnemonic, "bic") == 0 || shift;
    if (binary && (argc == 2 || argc == 3)) {
      const char *lhs_text = argc == 3 ? args[1] : args[0];
      const char *rhs_text = argc == 3 ? args[2] : args[1];
      unsigned lhs_bits = 64, rhs_bits = 64;
      int lhs = ny_native_asm_source(b, state, lhs_text, &lhs_bits);
      int rhs = lhs < 0 ? -1 : ny_native_asm_source(b, state, rhs_text, &rhs_bits);
      int value = -1;
      if (lhs < 0 || rhs < 0)
        return false;
      if (strcmp(mnemonic, "add") == 0) value = ny_native_asm_emit_binop(b, NYIR_ADD_I64, lhs, rhs);
      else if (strcmp(mnemonic, "sub") == 0) value = ny_native_asm_emit_binop(b, NYIR_SUB_I64, lhs, rhs);
      else if (strcmp(mnemonic, "and") == 0) value = ny_native_asm_emit_binop(b, NYIR_AND_I64, lhs, rhs);
      else if (strcmp(mnemonic, "eor") == 0) value = ny_native_asm_emit_binop(b, NYIR_XOR_I64, lhs, rhs);
      else if (strcmp(mnemonic, "orr") == 0) value = ny_native_asm_emit_binop(b, NYIR_OR_I64, lhs, rhs);
      else if (strcmp(mnemonic, "mul") == 0) value = ny_native_asm_emit_binop(b, NYIR_MUL_I64, lhs, rhs);
      else if (strcmp(mnemonic, "sdiv") == 0) value = ny_native_asm_sdiv(b, lhs, rhs);
      else if (strcmp(mnemonic, "udiv") == 0) value = ny_native_asm_udiv(b, lhs, rhs);
      else if (strcmp(mnemonic, "lsl") == 0) value = ny_native_asm_emit_binop(b, NYIR_SHL_I64, lhs, rhs);
      else if (strcmp(mnemonic, "asr") == 0) value = ny_native_asm_emit_binop(b, NYIR_SAR_I64, lhs, rhs);
      else if (strcmp(mnemonic, "lsr") == 0) value = ny_native_asm_lsr(b, lhs, rhs);
      else {
        int all = ny_native_nir_emit_const(b, -1);
        int inverted = all < 0 ? -1 : ny_native_asm_emit_binop(b, NYIR_XOR_I64, rhs, all);
        value = inverted < 0 ? -1 : ny_native_asm_emit_binop(b, NYIR_AND_I64, lhs, inverted);
      }
      if (value < 0 || !ny_native_asm_assign(b, state, args[0], value))
        return false;
      continue;
    }
    return ny_native_nir_fail(b, "native NYIR asm: unsupported AArch64 template instruction '%s'", mnemonic);
  }
  return true;
}

static int ny_native_nir_lower_aarch64_asm(ny_native_nir_builder_t *b,
                                            const expr_t *e) {
  ny_native_asm_state_t state;
  if (!ny_native_asm_parse_constraints(b, e, &state) ||
      !ny_native_asm_exec(b, &state, e->as.as_asm.code ? e->as.as_asm.code : ""))
    return -1;
  if (state.result < 0)
    return ny_native_nir_emit_const(b, 0);
  ny_native_asm_operand_t *result = &state.operands[state.result];
  if (!result->initialized)
    return ny_native_nir_fail(b, "native NYIR asm: output operand %d was not written", state.result), -1;
  return result->value;
}


/*
 * Lower a binary expression (arithmetic, comparison, power, string ops)
 * into NYIR.  Extracted from ny_native_nir_lower_expr.
 * See also: the remaining monolithic switch in ny_native_nir_lower_expr
 * still dispatches ~20 expression kinds inline.
 * Further extraction targets: ternary, logical, unary, match.
 */
