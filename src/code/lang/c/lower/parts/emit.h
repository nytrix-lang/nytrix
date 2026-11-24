/* IR emission and scalar, memory, and aggregate helpers, included by parse.c.
 */
static bool emit(c_parser_t *p, nyr_inst_t inst, int *value) {
  char local_error[256] = {0};
  if (p->abi == NYC_ABI_I386 && inst.op == NYIR_RET && p->function &&
      p->function->result != C_I64 && p->function->result != C_U64 &&
      !c_type_is_float(p->function->result))
    inst.flags |= NYIR_INST_F_I386_RET32;
  if (p->abi == NYC_ABI_I386 && inst.op == NYIR_RET && p->function &&
      p->function->result == C_AGG && p->function->variadic &&
      (p->function->has_regparm ? p->function->regparm : p->i386_regparm))
    inst.flags |= NYIR_INST_F_I386_SRET_CALLER_POP;
  /*
   * NYIR derives effects from opcode and flags, so volatility has to be
   * carried by the flag or it is lost when the instruction is emitted.
   */
  if ((inst.op == NYIR_LOAD_I64 || inst.op == NYIR_STORE_I64) &&
      (inst.effects & NYIR_EFFECT_VOLATILE))
    inst.flags |= NYIR_INST_F_MEM_VOLATILE;
  if (!nyr_emit(p->out, inst, value, local_error, sizeof(local_error))) {
    if (p->err && p->err_len && !p->err[0]) {
      if (!p->out) {
        ny_ctok_t token =
            p->pos < p->token_count ? p->tokens[p->pos] : (ny_ctok_t){0};
        const char *last_global =
            p->global_count && p->globals[p->global_count - 1].name.start
                ? p->globals[p->global_count - 1].name.start
                : "none";
        snprintf(p->err, p->err_len,
                 "instruction emission at file/function scope `%.48s` "
                 "after global `%.48s`, token %zu near `%.48s`: no active IR "
                 "function (%s)",
                 p->function && p->function->name.start
                     ? p->function->name.start
                     : "file scope",
                 last_global, p->pos, token.start ? token.start : "<end>",
                 local_error);
      } else {
        snprintf(p->err, p->err_len, "%s", local_error);
      }
    }
    return false;
  }
  return true;
}

static int emit_value(c_parser_t *p, nyr_inst_t inst) {
  int value = -1;
  return emit(p, inst, &value) ? value : -1;
}

static int emit_i64_constant(c_parser_t *p, int64_t value) {
  return emit_value(p, (nyr_inst_t){.op = NYIR_CONST_I64,
                                    .dst = -1,
                                    .imm = value,
                                    .scalar_class = NYIR_SCALAR_I64_RAW});
}

static int lower_truth_value(c_parser_t *p, c_value_t condition) {
  if (!c_type_is_float(condition.type))
    return condition.value;
  int zero = emit_value(
      p, (nyr_inst_t){.op = condition.type == C_F32 ? NYIR_CONST_F32
                                                    : NYIR_CONST_F64,
                      .dst = -1,
                      .scalar_class = c_float_scalar_class(condition.type)});
  return emit_value(
      p,
      (nyr_inst_t){.op = condition.type == C_F32 ? NYIR_CMP_F32 : NYIR_CMP_F64,
                   .dst = -1,
                   .a = condition.value,
                   .b = zero,
                   .scalar_class = NYIR_SCALAR_I64_RAW,
                   .cmp = NYIR_CMP_NE});
}

static int lower_boolean_value(c_parser_t *p, c_value_t value) {
  if (c_type_is_float(value.type))
    return lower_truth_value(p, value);
  int zero = emit_i64_constant(p, 0);
  return emit_value(p, (nyr_inst_t){.op = NYIR_CMP_I64,
                                    .dst = -1,
                                    .a = value.value,
                                    .b = zero,
                                    .cmp = NYIR_CMP_NE});
}

static unsigned c_function_regparm(const c_parser_t *p,
                                   const c_function_t *function) {
  return p->abi == NYC_ABI_I386 && !(function && function->variadic)
             ? function && function->has_regparm ? function->regparm
                                                 : p->i386_regparm
             : 0;
}

static bool lower_cleanup_local(c_parser_t *p, const c_local_t *local) {
  int address =
      local->type == C_AGG
          ? emit_value(p, (nyr_inst_t){.op = NYIR_LOAD_LOCAL,
                                       .dst = -1,
                                       .imm = (int64_t)local->slot,
                                       .scalar_class = NYIR_SCALAR_I64_RAW})
          : emit_value(p, (nyr_inst_t){.op = NYIR_ADDR_LOCAL,
                                       .dst = -1,
                                       .imm = (int64_t)local->slot,
                                       .scalar_class = NYIR_SCALAR_I64_RAW});
  if (address < 0)
    return false;
  nyr_inst_t call = {.op = NYIR_CALL,
                     .dst = -1,
                     .a = address,
                     .imm = 1,
                     .flags = NYIR_INST_F_EXTERN,
                     .effects = NYIR_EFFECT_CALL,
                     .symbol =
                         strndup(local->cleanup.start, local->cleanup.len)};
  if (!call.symbol) {
    fail(p, "out of memory creating cleanup call");
    return false;
  }
  nyr_call_arg_class_set(call.arg_scalar_classes, 0, NYIR_SCALAR_I64_RAW);
  call.flags |= c_function_regparm(p, find_function(p, local->cleanup))
                << NYIR_INST_F_I386_REGPARM_SHIFT;
  if (p->abi == NYC_ABI_I386)
    call.flags |= NYIR_INST_F_I386_C_ABI;
  bool emitted = emit(p, call, NULL);
  free((void *)call.symbol);
  return emitted;
}

static bool lower_cleanup_range(c_parser_t *p, size_t first, size_t last) {
  while (last > first) {
    const c_local_t *local = &p->locals[--last];
    if (local->cleanup.len && !lower_cleanup_local(p, local))
      return false;
  }
  return true;
}

/* Route only the taken edge through the cleanups. Compare expansion order;
 * macro-generated spellings can come from different backing buffers. */
static int lower_cleanup_edge(c_parser_t *p, const c_label_t *target) {
  /*
   * Every edge that leaves the same scopes toward the same label runs the
   * same cleanups; share one trampoline, as GCC merges those exits. Without
   * this each faulting access of a scoped user access gets its own copy of
   * the `clac`, and objtool sees a copy reachable with UACCESS already off.
   */
  size_t pending[NYC_CLEANUP_CACHE_LOCALS], pending_slots[NYC_CLEANUP_CACHE_LOCALS];
  size_t pending_count = 0;
  bool cacheable = true;
  {
    const c_cleanup_scope_t *probe = p->cleanup_scope;
    for (size_t i = p->local_count; i-- > p->param_count;) {
      const c_local_t *local = &p->locals[i];
      if (!local->cleanup.len)
        continue;
      while (probe && i < probe->first_local)
        probe = probe->parent;
      if ((!probe || target->token_pos < probe->end) &&
          target->token_pos >= local->declaration_pos)
        continue;
      if (pending_count == NYC_CLEANUP_CACHE_LOCALS) {
        cacheable = false;
        break;
      }
      pending[pending_count] = i;
      pending_slots[pending_count++] = (size_t)local->slot;
    }
  }
  if (cacheable && pending_count) {
    for (size_t e = 0; e < p->cleanup_cache_count; ++e) {
      const c_cleanup_cache_entry_t *entry = &p->cleanup_cache[e];
      if (entry->target == (int64_t)target->id &&
          entry->count == pending_count &&
          !memcmp(entry->locals, pending, pending_count * sizeof(size_t)) &&
          !memcmp(entry->slots, pending_slots,
                  pending_count * sizeof(size_t)))
        return entry->trampoline;
    }
  }
  const c_cleanup_scope_t *scope = p->cleanup_scope;
  int trampoline = -1, resume = -1;
  for (size_t i = p->local_count; i-- > p->param_count;) {
    const c_local_t *local = &p->locals[i];
    if (!local->cleanup.len)
      continue;
    while (scope && i < scope->first_local)
      scope = scope->parent;
    if ((!scope || target->token_pos < scope->end) &&
        target->token_pos >= local->declaration_pos)
      continue;
    if (trampoline < 0) {
      if (p->next_label > INT_MAX - 2) {
        fail(p, "C function has too many cleanup edges");
        return -1;
      }
      trampoline = p->next_label++;
      resume = p->next_label++;
      if (!emit_branch(p, resume) || !emit_label(p, trampoline))
        return -1;
    }
    if (!lower_cleanup_local(p, local))
      return -1;
  }
  if (trampoline < 0)
    return (int)target->id;
  if (!emit_branch(p, (int64_t)target->id) || !emit_label(p, resume))
    return -1;
  if (cacheable && pending_count &&
      p->cleanup_cache_count < NYC_CLEANUP_CACHE_ENTRIES) {
    c_cleanup_cache_entry_t *entry =
        &p->cleanup_cache[p->cleanup_cache_count++];
    entry->target = (int64_t)target->id;
    entry->count = pending_count;
    memcpy(entry->locals, pending, pending_count * sizeof(size_t));
    memcpy(entry->slots, pending_slots, pending_count * sizeof(size_t));
    entry->trampoline = trampoline;
  }
  return trampoline;
}

/*
 * NYIR stores integers as machine words. Keep C int values sign-extended in
 * that representation, truncating only at C int conversion boundaries.
 */
static int normalize_signed(c_parser_t *p, int value, unsigned bits) {
  int shift = emit_i64_constant(p, 64 - bits);
  if (shift < 0)
    return -1;
  int left = emit_i64_binary(p, NYIR_SHL_I64, value, shift);
  return left < 0 ? -1 : emit_i64_binary(p, NYIR_SAR_I64, left, shift);
}

static int normalize_i32(c_parser_t *p, int value) {
  return normalize_signed(p, value, 32);
}

static int normalize_i16(c_parser_t *p, int value) {
  return normalize_signed(p, value, 16);
}

static int normalize_u32(c_parser_t *p, int value) {
  int mask = emit_i64_constant(p, UINT32_MAX);
  return mask < 0 ? -1 : emit_i64_binary(p, NYIR_AND_I64, value, mask);
}

static int normalize_u16(c_parser_t *p, int value) {
  int mask = emit_i64_constant(p, UINT16_MAX);
  return mask < 0 ? -1 : emit_i64_binary(p, NYIR_AND_I64, value, mask);
}

static int normalize_u8(c_parser_t *p, int value) {
  int mask = emit_i64_constant(p, UINT8_MAX);
  return mask < 0 ? -1 : emit_i64_binary(p, NYIR_AND_I64, value, mask);
}

static int normalize_integer(c_parser_t *p, c_type_t type, int value) {
  switch (type) {
  case C_CHAR:
    return normalize_signed(p, value, 8);
  case C_I16:
    return normalize_i16(p, value);
  case C_I32:
    return normalize_i32(p, value);
  case C_U8:
    return normalize_u8(p, value);
  case C_U16:
    return normalize_u16(p, value);
  case C_U32:
    return normalize_u32(p, value);
  default:
    return value;
  }
}

static uint8_t c_local_memory_flags(const c_parser_t *p, c_type_t type) {
  return type == C_U8 || type == C_CHAR ? NYIR_INST_F_MEM_BYTE
         : type == C_I32 || type == C_U32 || type == C_F32 ||
                 (p && p->abi == NYC_ABI_I386 && c_type_is_pointer(type))
             ? NYIR_INST_F_MEM_I32
             : 0;
}

static unsigned c_pointer_offset_flags(const c_parser_t *p);

static int lower_load_u16_field(c_parser_t *p, int address, c_type_t type,
                                bool is_volatile);
static bool lower_store_u16_field(c_parser_t *p, int address, int value,
                                  c_type_t type, bool is_volatile);

static int lower_local_load(c_parser_t *p, const c_local_t *local) {
  if (c_type_is_complex(local->type) && !local->array_count) {
    fail(p, "complex values are not supported yet");
    return -1;
  }
  if (local->static_name.len) {
    int address = lower_static_local_address(p, local);
    if (address < 0 || local->array_count || local->type == C_AGG)
      return address;
    if (local->type == C_I16 || local->type == C_U16)
      return lower_load_u16_field(p, address, local->type, local->is_volatile);
    int value = emit_value(
        p, (nyr_inst_t){
               .op = NYIR_LOAD_I64,
               .dst = -1,
               .a = address,
               .effects = NYIR_EFFECT_READ_MEMORY | NYIR_EFFECT_MAY_TRAP |
                          (local->is_volatile ? NYIR_EFFECT_VOLATILE : 0),
               .flags = c_local_memory_flags(p, local->type) |
                        (local->is_volatile ? NYIR_INST_F_MEM_VOLATILE : 0),
               .scalar_class = c_type_is_float(local->type)
                                   ? c_float_scalar_class(local->type)
                                   : NYIR_SCALAR_I64_RAW});
    return value < 0 ? value : normalize_integer(p, local->type, value);
  }
  if (local->type == C_VA_LIST)
    return local->volatile_address;
  if (local->array_rank)
    return emit_value(p, (nyr_inst_t){.op = NYIR_LOAD_LOCAL,
                                      .dst = -1,
                                      .imm = (int64_t)local->slot,
                                      .scalar_class = NYIR_SCALAR_I64_RAW});
  if (!local->is_volatile && !local->address_taken) {
    int value = emit_value(
        p, (nyr_inst_t){.op = NYIR_LOAD_LOCAL,
                        .dst = -1,
                        .imm = (int64_t)local->slot,
                        .flags = c_type_is_pointer(local->type)
                                     ? c_pointer_offset_flags(p)
                                     : 0,
                        .scalar_class = c_type_is_float(local->type)
                                            ? c_float_scalar_class(local->type)
                                            : NYIR_SCALAR_I64_RAW});
    return value < 0 || !c_type_is_integer(local->type)
               ? value
               : normalize_integer(p, local->type, value);
  }
  int address = local->volatile_address;
  if (address < 0)
    address = emit_value(p, (nyr_inst_t){.op = NYIR_ADDR_LOCAL,
                                         .dst = -1,
                                         .imm = (int64_t)local->slot,
                                         .scalar_class = NYIR_SCALAR_I64_RAW});
  if (address < 0)
    return -1;
  if (local->type == C_I16 || local->type == C_U16)
    return lower_load_u16_field(p, address, local->type, local->is_volatile);
  int value = emit_value(
      p,
      (nyr_inst_t){.op = NYIR_LOAD_I64,
                   .dst = -1,
                   .a = address,
                   .effects = NYIR_EFFECT_READ_MEMORY | NYIR_EFFECT_MAY_TRAP |
                              (local->is_volatile ? NYIR_EFFECT_VOLATILE : 0),
                   .flags = c_local_memory_flags(p, local->type) |
                            (local->is_volatile ? NYIR_INST_F_MEM_VOLATILE : 0),
                   .scalar_class = c_type_is_float(local->type)
                                       ? c_float_scalar_class(local->type)
                                       : NYIR_SCALAR_I64_RAW});
  return value < 0 || !c_type_is_integer(local->type)
             ? value
             : normalize_integer(p, local->type, value);
}

static int lower_static_local_address(c_parser_t *p, const c_local_t *local) {
  static const char prefix[] = "__nyc_data_";
  if (!local || !local->static_name.len ||
      local->static_name.len > SIZE_MAX - sizeof(prefix)) {
    fail(p, "invalid static C object reference");
    return -1;
  }
  size_t symbol_len = sizeof(prefix) - 1 + local->static_name.len;
  char *symbol = malloc(symbol_len + 1);
  if (!symbol) {
    fail(p, "out of memory lowering a static C object reference");
    return -1;
  }
  memcpy(symbol, prefix, sizeof(prefix) - 1);
  memcpy(symbol + sizeof(prefix) - 1, local->static_name.start,
         local->static_name.len);
  symbol[symbol_len] = '\0';
  int address = emit_value(
      p, (nyr_inst_t){.op = NYIR_ADDR_SYMBOL, .dst = -1, .symbol = symbol});
  free(symbol);
  return address;
}

static bool lower_local_store(c_parser_t *p, c_local_t *local, int value) {
  local->has_integer_constant = false;
  if (local->static_name.len) {
    int address = lower_static_local_address(p, local);
    if (address < 0)
      return false;
    if (local->type == C_I16 || local->type == C_U16)
      return lower_store_u16_field(p, address, value, local->type,
                                   local->is_volatile);
    return emit(p,
                (nyr_inst_t){
                    .op = NYIR_STORE_I64,
                    .dst = -1,
                    .a = address,
                    .c = value,
                    .effects = NYIR_EFFECT_WRITE_MEMORY | NYIR_EFFECT_MAY_TRAP |
                               (local->is_volatile ? NYIR_EFFECT_VOLATILE : 0),
                    .flags = c_local_memory_flags(p, local->type),
                    .scalar_class = c_type_is_float(local->type)
                                        ? c_float_scalar_class(local->type)
                                        : NYIR_SCALAR_I64_RAW},
                NULL);
  }
  if (local->is_volatile || local->address_taken) {
    int address = local->volatile_address;
    if (address < 0)
      address =
          emit_value(p, (nyr_inst_t){.op = NYIR_ADDR_LOCAL,
                                     .dst = -1,
                                     .imm = (int64_t)local->slot,
                                     .scalar_class = NYIR_SCALAR_I64_RAW});
    if (address < 0)
      return false;
    if (local->type == C_I16 || local->type == C_U16)
      return lower_store_u16_field(p, address, value, local->type,
                                   local->is_volatile);
    return emit(p,
                (nyr_inst_t){
                    .op = NYIR_STORE_I64,
                    .dst = -1,
                    .a = address,
                    .c = value,
                    .effects = NYIR_EFFECT_WRITE_MEMORY | NYIR_EFFECT_MAY_TRAP |
                               (local->is_volatile ? NYIR_EFFECT_VOLATILE : 0),
                    .flags = c_local_memory_flags(p, local->type),
                    .scalar_class = c_type_is_float(local->type)
                                        ? c_float_scalar_class(local->type)
                                        : NYIR_SCALAR_I64_RAW},
                NULL);
  }
  return emit(
      p,
      (nyr_inst_t){.op = NYIR_STORE_LOCAL,
                   .dst = -1,
                   .a = value,
                   .imm = (int64_t)local->slot,
                   .scalar_class = c_type_is_float(local->type)
                                       ? c_float_scalar_class(local->type)
                                       : NYIR_SCALAR_I64_RAW},
      NULL);
}

static int lower_pointer_offset_sized(c_parser_t *p, int64_t stride, int base,
                                      int index, bool subtract);

static int lower_pointer_offset(c_parser_t *p, c_type_t pointer_type, int base,
                                int index, bool subtract) {
  return lower_pointer_offset_sized(p, c_pointer_scale(p->abi, pointer_type),
                                    base, index, subtract);
}

static int lower_pointer_offset_for(c_parser_t *p, c_type_t pointer_type,
                                    const c_agg_type_t *pointee, int base,
                                    int index, bool subtract) {
  return lower_pointer_offset_sized(
      p, c_pointer_scale_for(p->abi, pointer_type, pointee), base, index,
      subtract);
}

static bool c_i386_narrow32_operation(const c_parser_t *p, c_type_t type,
                                      nyr_op_t op) {
  if (!p || p->abi != NYC_ABI_I386 || (type != C_I32 && type != C_U32))
    return false;
  return op == NYIR_ADD_I64 || op == NYIR_SUB_I64 || op == NYIR_MUL_I64 ||
         op == NYIR_AND_I64 || op == NYIR_OR_I64 || op == NYIR_XOR_I64 ||
         op == NYIR_DIV_I64 || op == NYIR_MOD_I64 ||
         (type == C_I32 && (op == NYIR_SHL_I64 || op == NYIR_SAR_I64));
}

static unsigned c_pointer_offset_flags(const c_parser_t *p) {
  return p->abi == NYC_ABI_I386 ? NYIR_INST_F_NARROW32 : 0;
}

static int lower_pointer_offset_sized(c_parser_t *p, int64_t stride, int base,
                                      int index, bool subtract) {
  int scale = emit_value(
      p, (nyr_inst_t){.op = NYIR_CONST_I64, .dst = -1, .imm = stride});
  if (scale < 0)
    return -1;
  int offset = emit_value(
      p, (nyr_inst_t){.op = NYIR_MUL_I64,
                      .dst = -1,
                      .a = index,
                      .b = scale,
                      /* Pointer offsets on i386 are modulo 32 bits. */
                      .flags = c_pointer_offset_flags(p)});
  if (offset < 0)
    return -1;
  return emit_value(p,
                    (nyr_inst_t){.op = subtract ? NYIR_SUB_I64 : NYIR_ADD_I64,
                                 .dst = -1,
                                 .a = base,
                                 .b = offset,
                                 .flags = c_pointer_offset_flags(p)});
}

static int lower_pointer_index_address(c_parser_t *p, c_type_t pointer_type,
                                       int base, int index) {
  return lower_pointer_offset(p, pointer_type, base, index, false);
}

static int lower_pointer_index_address_for(c_parser_t *p, c_type_t pointer_type,
                                           const c_agg_type_t *pointee,
                                           int base, int index) {
  return lower_pointer_offset_for(p, pointer_type, pointee, base, index, false);
}

/*
 * Allocate `bytes` (8-byte aligned) of callee stack storage.  Each
 * allocation lowers to its own NYIR_ALLOCA; the result address is the
 * aggregate's base.
 */
static bool agg_area_alloc(c_parser_t *p, size_t bytes, size_t *offset_out) {
  size_t padded = align_up(bytes, 8);
  if (padded < bytes || padded > (size_t)INT32_MAX) {
    fail(p, "aggregate storage area exceeds the frontend limit");
    return false;
  }
  int base = emit_value(p, (nyr_inst_t){.op = NYIR_ALLOCA,
                                        .dst = -1,
                                        .imm = (int64_t)padded,
                                        .scalar_class = NYIR_SCALAR_I64_RAW});
  if (base < 0) {
    if (p->err && p->err_len && !p->err[0])
      fail(p, "aggregate storage allocation failed");
    return false;
  }
  p->agg_base = base;
  *offset_out = 0;
  return true;
}

static int lower_add_const(c_parser_t *p, int base, size_t offset) {
  if (offset == 0)
    return base;
  int constant = emit_value(
      p, (nyr_inst_t){.op = NYIR_CONST_I64, .dst = -1, .imm = (int64_t)offset});
  if (constant < 0)
    return -1;
  return emit_value(p, (nyr_inst_t){.op = NYIR_ADD_I64,
                                    .dst = -1,
                                    .a = base,
                                    .b = constant,
                                    .flags = c_pointer_offset_flags(p)});
}

static int lower_load_field(c_parser_t *p, int address, bool is_f64,
                            bool is_volatile) {
  return emit_value(
      p, (nyr_inst_t){
             .op = NYIR_LOAD_I64,
             .dst = -1,
             .a = address,
             .effects = NYIR_EFFECT_READ_MEMORY | NYIR_EFFECT_MAY_TRAP |
                        (is_volatile ? NYIR_EFFECT_VOLATILE : 0),
             .flags = is_volatile ? NYIR_INST_F_MEM_VOLATILE : 0,
             .scalar_class = is_f64 ? NYIR_SCALAR_F64 : NYIR_SCALAR_I64_RAW});
}

static int lower_load_pointer_field(c_parser_t *p, int address,
                                    bool is_volatile) {
  if (p->abi != NYC_ABI_I386)
    return lower_load_field(p, address, false, is_volatile);
  return emit_value(
      p,
      (nyr_inst_t){.op = NYIR_LOAD_I64,
                   .dst = -1,
                   .a = address,
                   .effects = NYIR_EFFECT_READ_MEMORY | NYIR_EFFECT_MAY_TRAP |
                              (is_volatile ? NYIR_EFFECT_VOLATILE : 0),
                   .flags = NYIR_INST_F_MEM_I32 |
                            (is_volatile ? NYIR_INST_F_MEM_VOLATILE : 0),
                   .scalar_class = NYIR_SCALAR_I64_RAW});
}

static int lower_load_f32_field(c_parser_t *p, int address, bool is_volatile) {
  return emit_value(
      p,
      (nyr_inst_t){.op = NYIR_LOAD_I64,
                   .dst = -1,
                   .a = address,
                   .effects = NYIR_EFFECT_READ_MEMORY | NYIR_EFFECT_MAY_TRAP |
                              (is_volatile ? NYIR_EFFECT_VOLATILE : 0),
                   .flags = NYIR_INST_F_MEM_I32 |
                            (is_volatile ? NYIR_INST_F_MEM_VOLATILE : 0),
                   .scalar_class = NYIR_SCALAR_F32});
}

static bool lower_store_field(c_parser_t *p, int address, int value,
                              bool is_f64, bool is_volatile) {
  return emit(p,
              (nyr_inst_t){.op = NYIR_STORE_I64,
                           .dst = -1,
                           .a = address,
                           .c = value,
                           .effects = NYIR_EFFECT_WRITE_MEMORY |
                                      NYIR_EFFECT_MAY_TRAP |
                                      (is_volatile ? NYIR_EFFECT_VOLATILE : 0),
                           .flags = is_volatile ? NYIR_INST_F_MEM_VOLATILE : 0,
                           .scalar_class =
                               is_f64 ? NYIR_SCALAR_F64 : NYIR_SCALAR_I64_RAW},
              NULL);
}

static bool lower_store_pointer_field(c_parser_t *p, int address, int value,
                                      bool is_volatile) {
  if (p->abi != NYC_ABI_I386)
    return lower_store_field(p, address, value, false, is_volatile);
  return emit(
      p,
      (nyr_inst_t){.op = NYIR_STORE_I64,
                   .dst = -1,
                   .a = address,
                   .c = value,
                   .effects = NYIR_EFFECT_WRITE_MEMORY | NYIR_EFFECT_MAY_TRAP |
                              (is_volatile ? NYIR_EFFECT_VOLATILE : 0),
                   .flags = NYIR_INST_F_MEM_I32 |
                            (is_volatile ? NYIR_INST_F_MEM_VOLATILE : 0),
                   .scalar_class = NYIR_SCALAR_I64_RAW},
      NULL);
}

static bool lower_store_f32_field(c_parser_t *p, int address, int value,
                                  bool is_volatile) {
  return emit(
      p,
      (nyr_inst_t){.op = NYIR_STORE_I64,
                   .dst = -1,
                   .a = address,
                   .c = value,
                   .effects = NYIR_EFFECT_WRITE_MEMORY | NYIR_EFFECT_MAY_TRAP |
                              (is_volatile ? NYIR_EFFECT_VOLATILE : 0),
                   .flags = NYIR_INST_F_MEM_I32 |
                            (is_volatile ? NYIR_INST_F_MEM_VOLATILE : 0),
                   .scalar_class = NYIR_SCALAR_F32},
      NULL);
}

static c_agg_type_t lower_complex_value_f32 = {
    .scope_id = SIZE_MAX,
    .complete = true,
    .size = 8,
    .align = 4,
    .field_count = 2,
    .fields = {{.kind = C_AGG_FIELD_F32, .offset = 0, .size = 4, .align = 4},
               {.kind = C_AGG_FIELD_F32, .offset = 4, .size = 4, .align = 4}}};
static c_agg_type_t lower_complex_value_f64 = {
    .scope_id = SIZE_MAX,
    .complete = true,
    .size = 16,
    .align = 8,
    .field_count = 2,
    .fields = {{.kind = C_AGG_FIELD_F64, .offset = 0, .size = 8, .align = 8},
               {.kind = C_AGG_FIELD_F64, .offset = 8, .size = 8, .align = 8}}};

/*
 * Promote a real operand of a mixed arithmetic expression to the complex shape
 * the other operand requires: real lane gets the value, imaginary lane zero.
 */
static c_value_t lower_scalar_to_complex(c_parser_t *p, c_value_t value,
                                         bool single) {
  size_t offset = 0;
  if (!agg_area_alloc(p, single ? 8 : 16, &offset))
    return (c_value_t){-1, C_BAD, NULL};
  int base = p->agg_base;
  int real = value.value;
  if (single && value.type == C_F64)
    real = emit_value(p, (nyr_inst_t){.op = NYIR_F64_TO_F32,
                                      .dst = -1,
                                      .a = value.value,
                                      .scalar_class = NYIR_SCALAR_F32});
  else if (!single && value.type == C_F32)
    real = emit_value(p, (nyr_inst_t){.op = NYIR_F32_TO_F64,
                                      .dst = -1,
                                      .a = value.value,
                                      .scalar_class = NYIR_SCALAR_F64});
  else if (c_type_is_integer(value.type))
    real = emit_value(
        p, (nyr_inst_t){.op = single ? NYIR_I64_TO_F32 : NYIR_I64_TO_F64,
                        .dst = -1,
                        .a = value.value,
                        .scalar_class =
                            single ? NYIR_SCALAR_F32 : NYIR_SCALAR_F64});
  int zero =
      emit_value(p, (nyr_inst_t){.op = single ? NYIR_CONST_F32 : NYIR_CONST_F64,
                                 .dst = -1,
                                 .imm = 0,
                                 .scalar_class = single ? NYIR_SCALAR_F32
                                                        : NYIR_SCALAR_F64});
  int imag = lower_add_const(p, base, single ? 4 : 8);
  if (real < 0 || zero < 0 || imag < 0)
    return (c_value_t){-1, C_BAD, NULL};
  bool stored = single ? lower_store_f32_field(p, base, real, false) &&
                             lower_store_f32_field(p, imag, zero, false)
                       : lower_store_field(p, base, real, true, false) &&
                             lower_store_field(p, imag, zero, true, false);
  if (!stored)
    return (c_value_t){-1, C_BAD, NULL};
  return (c_value_t){base, C_AGG,
                     single ? &lower_complex_value_f32
                            : &lower_complex_value_f64};
}

static int lower_load_i32_field(c_parser_t *p, int address, bool is_volatile) {
  int loaded = emit_value(
      p,
      (nyr_inst_t){.op = NYIR_LOAD_I64,
                   .dst = -1,
                   .a = address,
                   .effects = NYIR_EFFECT_READ_MEMORY | NYIR_EFFECT_MAY_TRAP |
                              (is_volatile ? NYIR_EFFECT_VOLATILE : 0),
                   .flags = NYIR_INST_F_MEM_I32 |
                            (is_volatile ? NYIR_INST_F_MEM_VOLATILE : 0),
                   .scalar_class = NYIR_SCALAR_I64_RAW});
  return loaded < 0 ? -1 : normalize_i32(p, loaded);
}

static int lower_load_u8_field_seg(c_parser_t *p, int address, bool is_volatile,
                                   unsigned seg_flags) {
  return emit_value(
      p,
      (nyr_inst_t){.op = NYIR_LOAD_I64,
                   .dst = -1,
                   .a = address,
                   .effects = NYIR_EFFECT_READ_MEMORY | NYIR_EFFECT_MAY_TRAP |
                              (is_volatile ? NYIR_EFFECT_VOLATILE : 0),
                   .flags = NYIR_INST_F_MEM_BYTE | seg_flags |
                            (is_volatile ? NYIR_INST_F_MEM_VOLATILE : 0),
                   .scalar_class = NYIR_SCALAR_I64_RAW});
}

static int lower_load_u8_field(c_parser_t *p, int address, bool is_volatile) {
  return lower_load_u8_field_seg(p, address, is_volatile, 0);
}

static int lower_load_u16_field_seg(c_parser_t *p, int address, c_type_t type,
                                    bool is_volatile, unsigned seg_flags) {
  if (p->abi == NYC_ABI_I386 ||
      (is_volatile && p->abi == NYC_ABI_SYSV_X86_64)) {
    int loaded = emit_value(
        p,
        (nyr_inst_t){.op = NYIR_LOAD_I64,
                     .dst = -1,
                     .a = address,
                     .effects = NYIR_EFFECT_READ_MEMORY | NYIR_EFFECT_MAY_TRAP |
                                (is_volatile ? NYIR_EFFECT_VOLATILE : 0),
                     .flags = NYIR_INST_F_MEM_U16 | seg_flags |
                              (is_volatile ? NYIR_INST_F_MEM_VOLATILE : 0),
                     .scalar_class = NYIR_SCALAR_I64_RAW});
    return loaded < 0      ? -1
           : type == C_I16 ? normalize_i16(p, loaded)
                           : normalize_u16(p, loaded);
  }
  int high_address = lower_add_const(p, address, 1);
  int low = lower_load_u8_field_seg(p, address, is_volatile, seg_flags);
  int high = high_address < 0 ? -1
                              : lower_load_u8_field_seg(p, high_address,
                                                        is_volatile, seg_flags);
  int shift = high < 0 ? -1 : emit_i64_constant(p, 8);
  int upper = shift < 0 ? -1 : emit_i64_binary(p, NYIR_SHL_I64, high, shift);
  int joined =
      low < 0 || upper < 0 ? -1 : emit_i64_binary(p, NYIR_OR_I64, low, upper);
  return joined < 0      ? -1
         : type == C_I16 ? normalize_i16(p, joined)
                         : normalize_u16(p, joined);
}

static int lower_load_u16_field(c_parser_t *p, int address, c_type_t type,
                                bool is_volatile) {
  return lower_load_u16_field_seg(p, address, type, is_volatile, 0);
}

static int lower_load_atomic_u16(c_parser_t *p, int address) {
  return emit_value(
      p, (nyr_inst_t){.op = NYIR_LOAD_I64,
                      .dst = -1,
                      .a = address,
                      .effects = NYIR_EFFECT_READ_MEMORY |
                                 NYIR_EFFECT_MAY_TRAP | NYIR_EFFECT_VOLATILE,
                      .flags = NYIR_INST_F_MEM_U16 | NYIR_INST_F_MEM_VOLATILE,
                      .scalar_class = NYIR_SCALAR_I64_RAW});
}

static bool lower_store_i32_field(c_parser_t *p, int address, int value,
                                  bool is_volatile) {
  return emit(
      p,
      (nyr_inst_t){.op = NYIR_STORE_I64,
                   .dst = -1,
                   .a = address,
                   .c = value,
                   .effects = NYIR_EFFECT_WRITE_MEMORY | NYIR_EFFECT_MAY_TRAP |
                              (is_volatile ? NYIR_EFFECT_VOLATILE : 0),
                   .flags = NYIR_INST_F_MEM_I32 |
                            (is_volatile ? NYIR_INST_F_MEM_VOLATILE : 0),
                   .scalar_class = NYIR_SCALAR_I64_RAW},
      NULL);
}

static bool lower_store_u8_field_seg(c_parser_t *p, int address, int value,
                                     bool is_volatile, unsigned seg_flags) {
  value = normalize_integer(p, C_U8, value);
  if (value < 0)
    return false;
  return emit(
      p,
      (nyr_inst_t){.op = NYIR_STORE_I64,
                   .dst = -1,
                   .a = address,
                   .c = value,
                   .effects = NYIR_EFFECT_WRITE_MEMORY | NYIR_EFFECT_MAY_TRAP |
                              (is_volatile ? NYIR_EFFECT_VOLATILE : 0),
                   .flags = NYIR_INST_F_MEM_BYTE | seg_flags |
                            (is_volatile ? NYIR_INST_F_MEM_VOLATILE : 0),
                   .scalar_class = NYIR_SCALAR_I64_RAW},
      NULL);
}

static bool lower_store_u8_field(c_parser_t *p, int address, int value,
                                 bool is_volatile) {
  return lower_store_u8_field_seg(p, address, value, is_volatile, 0);
}

static bool lower_store_u16_field_seg(c_parser_t *p, int address, int value,
                                      c_type_t type, bool is_volatile,
                                      unsigned seg_flags) {
  value = normalize_u16(p, value);
  if (type == C_I16)
    value = normalize_i16(p, value);
  if (p->abi == NYC_ABI_I386 ||
      (is_volatile && p->abi == NYC_ABI_SYSV_X86_64)) {
    if (value < 0)
      return false;
    return emit(p,
                (nyr_inst_t){
                    .op = NYIR_STORE_I64,
                    .dst = -1,
                    .a = address,
                    .c = value,
                    .effects = NYIR_EFFECT_WRITE_MEMORY | NYIR_EFFECT_MAY_TRAP |
                               (is_volatile ? NYIR_EFFECT_VOLATILE : 0),
                    .flags = NYIR_INST_F_MEM_U16 | seg_flags |
                             (is_volatile ? NYIR_INST_F_MEM_VOLATILE : 0),
                    .scalar_class = NYIR_SCALAR_I64_RAW},
                NULL);
  }
  int high_address = lower_add_const(p, address, 1);
  int shift = value < 0 ? -1 : emit_i64_constant(p, 8);
  int high =
      shift < 0
          ? -1
          : emit_value(p, (nyr_inst_t){.op = NYIR_SAR_I64,
                                       .dst = -1,
                                       .a = value,
                                       .b = shift,
                                       .scalar_class = NYIR_SCALAR_I64_RAW});
  if (value < 0 || high < 0 || high_address < 0)
    return false;
  return lower_store_u8_field_seg(p, address, value, is_volatile, seg_flags) &&
         lower_store_u8_field_seg(p, high_address, high, is_volatile,
                                  seg_flags);
}

static bool lower_store_u16_field(c_parser_t *p, int address, int value,
                                  c_type_t type, bool is_volatile) {
  return lower_store_u16_field_seg(p, address, value, type, is_volatile, 0);
}

static int lower_bitfield_binary(c_parser_t *p, nyr_op_t op, int value,
                                 uint64_t constant) {
  if (value < 0)
    return -1;
  int rhs = emit_value(
      p,
      (nyr_inst_t){.op = NYIR_CONST_I64, .dst = -1, .imm = (int64_t)constant});
  return rhs < 0
             ? -1
             : emit_value(p, (nyr_inst_t){.op = op,
                                          .dst = -1,
                                          .a = value,
                                          .b = rhs,
                                          .scalar_class = NYIR_SCALAR_I64_RAW});
}

static int lower_bitfield_value(c_parser_t *p, const c_agg_field_t *field,
                                int value) {
  unsigned width = field->bit_width;
  if (width == 64)
    return value;
  if (field->kind == C_AGG_FIELD_I32 || field->kind == C_AGG_FIELD_I64 ||
      field->kind == C_AGG_FIELD_I16) {
    value = lower_bitfield_binary(p, NYIR_SHL_I64, value, 64 - width);
    return lower_bitfield_binary(p, NYIR_SAR_I64, value, 64 - width);
  }
  return lower_bitfield_binary(p, NYIR_AND_I64, value,
                               (UINT64_C(1) << width) - 1);
}

static int lower_load_bitfield(c_parser_t *p, int address,
                               const c_agg_field_t *field) {
  int value = field->size == 1
                  ? lower_load_u8_field(p, address, field->is_volatile)
              : field->size == 2
                  ? lower_load_u16_field(p, address, C_U16, field->is_volatile)
              : field->size == 4
                  ? lower_load_i32_field(p, address, field->is_volatile)
                  : lower_load_field(p, address, false, field->is_volatile);
  if (field->bit_offset)
    value = lower_bitfield_binary(p, NYIR_SAR_I64, value, field->bit_offset);
  return lower_bitfield_value(p, field, value);
}

static bool lower_store_bitfield(c_parser_t *p, int address,
                                 const c_agg_field_t *field, int value) {
  uint64_t mask = field->bit_width == 64
                      ? UINT64_MAX
                      : (UINT64_C(1) << field->bit_width) - 1;
  int bits = lower_bitfield_binary(p, NYIR_AND_I64, value, mask);
  if (field->bit_offset)
    bits = lower_bitfield_binary(p, NYIR_SHL_I64, bits, field->bit_offset);
  int prior = field->size == 1
                  ? lower_load_u8_field(p, address, field->is_volatile)
              : field->size == 2
                  ? lower_load_u16_field(p, address, C_U16, field->is_volatile)
              : field->size == 4
                  ? lower_load_i32_field(p, address, field->is_volatile)
                  : lower_load_field(p, address, false, field->is_volatile);
  prior = lower_bitfield_binary(p, NYIR_AND_I64, prior,
                                ~(mask << field->bit_offset));
  if (bits < 0 || prior < 0)
    return false;
  int merged = emit_i64_binary(p, NYIR_OR_I64, prior, bits);
  return merged >= 0 &&
         (field->size == 1
              ? lower_store_u8_field(p, address, merged, field->is_volatile)
          : field->size == 2 ? lower_store_u16_field(p, address, merged, C_U16,
                                                     field->is_volatile)
          : field->size == 4
              ? lower_store_i32_field(p, address, merged, field->is_volatile)
              : lower_store_field(p, address, merged, false,
                                  field->is_volatile));
}

static int lower_load_segmented_global(c_parser_t *p, const c_global_t *global,
                                       int address) {
  c_type_t type = global->type;
  unsigned flags = c_seg_flags(global->object_seg);
  if (type == C_I16 || type == C_U16)
    return lower_load_u16_field_seg(p, address, type, global->is_volatile,
                                    flags);
  if (type == C_I32 || type == C_U32)
    flags |= NYIR_INST_F_MEM_I32;
  else if (type == C_U8 || type == C_CHAR)
    flags |= NYIR_INST_F_MEM_BYTE;
  return emit_value(
      p, (nyr_inst_t){
             .op = NYIR_LOAD_I64,
             .dst = -1,
             .a = address,
             .effects = NYIR_EFFECT_READ_MEMORY | NYIR_EFFECT_MAY_TRAP |
                        (global->is_volatile ? NYIR_EFFECT_VOLATILE : 0),
             .flags = flags,
             .scalar_class = c_type_is_float(type) ? c_float_scalar_class(type)
                                                   : NYIR_SCALAR_I64_RAW});
}

static bool lower_copy_struct(c_parser_t *p, int dst, int src, size_t size) {
  return emit(p,
              (nyr_inst_t){.op = NYIR_COPY_STRUCT,
                           .dst = -1,
                           .a = dst,
                           .b = src,
                           .imm = (int64_t)size,
                           .effects = NYIR_EFFECT_READ_MEMORY |
                                      NYIR_EFFECT_WRITE_MEMORY},
              NULL);
}

static bool lower_zero_fill(c_parser_t *p, int address, size_t size) {
  if (size >= 128 && size <= INT64_MAX && !p->no_builtin) {
    int zero = emit_i64_constant(p, 0);
    int length = emit_i64_constant(p, (int64_t)size);
    if (zero < 0 || length < 0)
      return false;
    nyr_inst_t call = {.op = NYIR_CALL,
                       .dst = -1,
                       .a = address,
                       .b = zero,
                       .c = length,
                       .d = -1,
                       .e = -1,
                       .f = -1,
                       .imm = 3,
                       .flags = NYIR_INST_F_EXTERN,
                       .effects = NYIR_EFFECT_CALL | NYIR_EFFECT_WRITE_MEMORY |
                                  NYIR_EFFECT_MAY_TRAP,
                       .symbol = "memset",
                       .scalar_class = NYIR_SCALAR_I64_RAW};
    for (int i = 0; i < 3; ++i)
      nyr_call_arg_class_set(call.arg_scalar_classes, i, NYIR_SCALAR_I64_RAW);
    if (p->abi == NYC_ABI_I386)
      call.flags |= NYIR_INST_F_I386_C_ABI;
    return emit(p, call, NULL);
  }
  int zero = emit_i64_constant(p, 0);
  if (zero < 0)
    return false;
  size_t words = align_up(size, 8) / 8;
  for (size_t word = 0; word < words; ++word) {
    int target = word ? lower_add_const(p, address, word * 8) : address;
    if (target < 0 || !lower_store_field(p, target, zero, false, false))
      return false;
  }
  return true;
}

static const c_agg_field_t *find_field(const c_agg_type_t *agg,
                                       ny_ctok_t name) {
  if (!agg)
    return NULL;
  for (size_t i = 0; i < agg->field_count; ++i)
    if (tok_same(agg->fields[i].name, name))
      return &agg->fields[i];
  for (size_t i = 0; i < agg->promoted_field_count; ++i)
    if (tok_same(agg->promoted_fields[i].name, name))
      return &agg->promoted_fields[i];
  return NULL;
}

static void find_boot_cpu_capability(c_parser_t *p,
                                     c_global_t *fallback_boot_cpu,
                                     c_agg_field_t *fallback_capability,
                                     c_global_t **boot_cpu,
                                     const c_agg_field_t **capability) {
  static const ny_ctok_t boot_cpu_name = {.start = "boot_cpu_data",
                                          .len = sizeof("boot_cpu_data") - 1,
                                          .kind = NY_CTOK_IDENT};
  static const ny_ctok_t cpuinfo_name = {.start = "cpuinfo_x86",
                                         .len = sizeof("cpuinfo_x86") - 1,
                                         .kind = NY_CTOK_IDENT};
  static const ny_ctok_t capability_name = {.start = "x86_capability",
                                            .len = sizeof("x86_capability") - 1,
                                            .kind = NY_CTOK_IDENT};
  *boot_cpu = find_global(p, boot_cpu_name);
  *capability = *boot_cpu && (*boot_cpu)->type == C_AGG && (*boot_cpu)->agg
                    ? find_field((*boot_cpu)->agg, capability_name)
                    : NULL;
  if (*boot_cpu)
    return;

  c_agg_type_t *cpuinfo = find_tag(p, cpuinfo_name, false);
  if (cpuinfo) {
    *fallback_boot_cpu = (c_global_t){.name = boot_cpu_name,
                                      .type = C_AGG,
                                      .agg = cpuinfo,
                                      .is_external = true};
    *capability = find_field(cpuinfo, capability_name);
  } else {
    bool vmx_feature_names = false;
    static const char vmx_macro[] = "CONFIG_X86_VMX_FEATURE_NAMES";
    for (size_t i = 0; i < p->pp_constant_count; ++i)
      if (p->pp_constants[i].name_len == sizeof(vmx_macro) - 1 &&
          memcmp(p->pp_constants[i].name, vmx_macro, sizeof(vmx_macro) - 1) ==
              0 &&
          p->pp_constants[i].integer_value && p->pp_constants[i].value != 0) {
        vmx_feature_names = true;
        break;
      }
    /*
     * page_64.h emits the LA57 alternative without including cpuinfo_x86.
     * Its x86-64 capability array starts at byte 24, or byte 48 when
     * CONFIG_X86_VMX_FEATURE_NAMES adds five words before it.
     */
    *fallback_boot_cpu =
        (c_global_t){.name = boot_cpu_name, .type = C_AGG, .is_external = true};
    *fallback_capability =
        (c_agg_field_t){.size = 96, .offset = vmx_feature_names ? 48 : 24};
    *capability = fallback_capability;
  }
  *boot_cpu = fallback_boot_cpu;
}

/*
 * Mark the end of a path that cannot continue (a noreturn call or
 * __builtin_unreachable). The driver removes the code behind it and leaves
 * the marker out of the assembly, so no instruction follows the call as in
 * GCC output; objtool reports such instructions as unreachable.
 */
static bool emit_noreturn_marker(c_parser_t *p) {
  return emit(p,
              (nyr_inst_t){.op = NYIR_INLINE_ASM,
                           .dst = -1,
                           .a = -1,
                           .b = -1,
                           .c = -1,
                           .d = -1,
                           .e = -1,
                           .f = -1,
                           .symbol = "ud2 # noreturn",
                           .effects = NYIR_EFFECT_CONTROL |
                                      NYIR_EFFECT_MAY_TRAP},
              NULL);
}
