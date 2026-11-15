/* Local initializer and declaration lowering, included by parse.c. */
static bool lower_initializer_field(c_parser_t *p, const c_agg_field_t *field,
                                    int base, size_t offset) {
  static const c_type_t types[] = {C_I64, C_U64, C_I32, C_U8,  C_I16,
                                   C_U16, C_U32, C_F32, C_F64, C_VOID_PTR};
  if ((size_t)field->kind >= sizeof(types) / sizeof(types[0])) {
    fail(p, "unsupported scalar aggregate initializer");
    return false;
  }
  c_type_t type = types[field->kind];
  c_value_t value = parse_expr(p, 1);
  if (value.value < 0)
    return false;
  bool pointer_to_bool = field->is_bool && c_type_is_pointer(value.type);
  if (type == C_VOID_PTR) {
    if (!c_type_is_pointer(value.type) &&
        !p->last_value_null_pointer_constant) {
      fail(p, "pointer field initializer requires a pointer or zero");
      return false;
    }
  } else if (pointer_to_bool) {
    int zero = emit_i64_constant(p, 0);
    value.value =
        zero < 0 ? -1 : emit_i64_compare(p, value.value, zero, NYIR_CMP_NE);
    value.type = C_U8;
    if (value.value < 0)
      return false;
  } else if (!c_type_is_integer(value.type) && !c_type_is_float(value.type)) {
    fail(p, "numeric field initializer requires an arithmetic value");
    return false;
  } else if (c_type_is_float(type)) {
    value = convert_numeric_float(p, value, type);
  } else {
    if (c_type_is_float(value.type)) {
      if (type == C_U64) {
        nyr_inst_t call = {.op = NYIR_CALL,
                           .dst = -1,
                           .a = value.value,
                           .imm = 1,
                           .flags = NYIR_INST_F_EXTERN,
                           .effects = NYIR_EFFECT_CALL,
                           .scalar_class = NYIR_SCALAR_I64_RAW,
                           .symbol = value.type == C_F32 ? "__fixunssfdi"
                                                         : "__fixunsdfdi"};
        nyr_call_arg_class_set(call.arg_scalar_classes, 0,
                               c_float_scalar_class(value.type));
        value.value = emit_value(p, call);
      } else {
        value.value = emit_value(
            p, (nyr_inst_t){.op = value.type == C_F32 ? NYIR_F32_TO_I64
                                                      : NYIR_F64_TO_I64,
                            .dst = -1,
                            .a = value.value,
                            .scalar_class = NYIR_SCALAR_I64_RAW});
      }
    }
    if (value.value >= 0)
      value.value = normalize_integer(p, type, value.value);
  }
  if (value.value < 0)
    return false;
  int address = lower_add_const(p, base, offset);
  if (address < 0)
    return false;
  if (field->is_bitfield)
    return lower_store_bitfield(p, address, field, value.value);
  if (type == C_I16 || type == C_U16)
    return lower_store_u16_field(p, address, value.value, type,
                                 field->is_volatile);
  return emit(
      p,
      (nyr_inst_t){.op = NYIR_STORE_I64,
                   .dst = -1,
                   .a = address,
                   .c = value.value,
                   .flags = c_local_memory_flags(p, type),
                   .effects = NYIR_EFFECT_WRITE_MEMORY | NYIR_EFFECT_MAY_TRAP |
                              (field->is_volatile ? NYIR_EFFECT_VOLATILE : 0),
                   .scalar_class = c_type_is_float(type)
                                       ? c_float_scalar_class(type)
                                       : NYIR_SCALAR_I64_RAW},
      NULL);
}

static bool add_local(c_parser_t *p, ny_ctok_t name, c_type_t type, int value,
                      bool is_const, bool is_volatile) {
  for (size_t i = p->local_count; i > p->scope_start; --i) {
    if (tok_same(p->locals[i - 1].name, name)) {
      fail(p, "duplicate declaration in the same C scope");
      return false;
    }
  }
  if (p->local_count >= p->local_capacity || p->next_local == SIZE_MAX) {
    fail(p, "C frontend local-variable storage capacity exceeded");
    return false;
  }
  size_t slot = p->next_local++;
  if (type == C_VA_LIST && value >= 0) {
    fail(p, "va_list local initializers are unsupported");
    return false;
  }
  int stored_value = value >= 0 ? normalize_integer(p, type, value) : value;
  if (value >= 0 && stored_value < 0)
    return false;
  int volatile_address = -1;
  if (is_volatile || type == C_VA_LIST) {
    volatile_address =
        emit_value(p, (nyr_inst_t){.op = NYIR_ALLOCA,
                                   .dst = -1,
                                   .imm = type == C_VA_LIST ? 24 : 8,
                                   .scalar_class = NYIR_SCALAR_I64_RAW});
    if (volatile_address < 0)
      return false;
  }
  if (stored_value >= 0 &&
      !emit(p,
            is_volatile
                ? (nyr_inst_t){.op = NYIR_STORE_I64,
                               .dst = -1,
                               .a = volatile_address,
                               .c = stored_value,
                               .effects = NYIR_EFFECT_WRITE_MEMORY |
                                          NYIR_EFFECT_MAY_TRAP |
                                          NYIR_EFFECT_VOLATILE,
                               .flags = c_local_memory_flags(p, type),
                               .scalar_class = c_type_is_float(type)
                                                   ? c_float_scalar_class(type)
                                                   : NYIR_SCALAR_I64_RAW}
                : (nyr_inst_t){.op = NYIR_STORE_LOCAL,
                               .dst = -1,
                               .a = stored_value,
                               .imm = (int64_t)slot,
                               .scalar_class = c_type_is_float(type)
                                                   ? c_float_scalar_class(type)
                                                   : NYIR_SCALAR_I64_RAW},
            NULL))
    return false;
  p->locals[p->local_count++] =
      (c_local_t){.name = name,
                  .type = type,
                  .is_long_long = p->last_type_long_long,
                  .slot = slot,
                  .is_const = is_const,
                  .is_volatile = is_volatile,
                  .volatile_address = volatile_address};
  return true;
}

static bool parse_static_array_initializer(
    c_parser_t *p, c_type_t type, c_global_t *data, unsigned char *bytes,
    size_t element_count, size_t *element_index, unsigned depth,
    bool allow_designators, size_t designator_stride) {
  if (depth > 2 || !expect(p, "{"))
    return false;
  size_t width = c_type_size(p->abi, type);
  while (!tok_is(p, p->pos, "}")) {
    if (tok_is(p, p->pos, "{")) {
      size_t nested_end = *element_index;
      if (designator_stride > element_count - nested_end)
        nested_end = element_count;
      else
        nested_end += designator_stride;
      if (!parse_static_array_initializer(p, type, data, bytes, nested_end,
                                          element_index, depth + 1,
                                          allow_designators, 1))
        return false;
      *element_index = nested_end;
    } else {
      size_t range_first = 0;
      size_t range_last = 0;
      if (accept(p, "[")) {
        int64_t designated_index = 0;
        int64_t designated_last = 0;
        if (!allow_designators ||
            !parse_global_const_expr(p, &p->pos, &designated_index) ||
            designated_index < 0) {
          fail(p, "unsupported or out-of-range static array designator");
          return false;
        }
        designated_last = designated_index;
        if (accept(p, "...")) {
          if (!parse_global_const_expr(p, &p->pos, &designated_last) ||
              designated_last < designated_index) {
            fail(p, "invalid static array range designator");
            return false;
          }
        }
        if (!expect(p, "]") || !expect(p, "=") ||
            (uint64_t)designated_last >= element_count / designator_stride) {
          fail(p, "unsupported or out-of-range static array designator");
          return false;
        }
        range_first = (size_t)designated_index * designator_stride;
        range_last = ((size_t)designated_last + 1) * designator_stride;
        *element_index = range_first;
        if (tok_is(p, p->pos, "{")) {
          size_t nested_index = range_first;
          if (!parse_static_array_initializer(p, type, data, bytes, range_last,
                                              &nested_index, depth + 1,
                                              allow_designators, 1))
            return false;
          *element_index = range_last;
          if (!accept(p, ",") && !tok_is(p, p->pos, "}")) {
            fail(p, "expected ',' in static array initializer");
            return false;
          }
          continue;
        }
      } else {
        range_first = *element_index;
        range_last = range_first + 1;
      }
      if (depth == 0 && designator_stride > 1 &&
          (type == C_CHAR || type == C_U8) && p->pos < p->token_count &&
          p->tokens[p->pos].kind == NY_CTOK_STRING) {
        unsigned char *row = bytes + range_first * width;
        size_t used = 0;
        while (p->pos < p->token_count &&
               p->tokens[p->pos].kind == NY_CTOK_STRING) {
          unsigned char *part = NULL;
          size_t part_len = 0;
          if (!decode_string_token(p->tokens[p->pos++], &part, &part_len) ||
              !part_len || used >= designator_stride ||
              part_len - 1 > designator_stride - used - 1) {
            free(part);
            fail(p, "static string row does not fit its character array");
            return false;
          }
          memcpy(row + used, part, part_len - 1);
          used += part_len - 1;
          free(part);
        }
        row[used] = 0;
        for (size_t offset = range_first + designator_stride;
             offset < range_last; offset += designator_stride)
          memcpy(bytes + offset * width, row, designator_stride * width);
        *element_index = range_last;
        if (!accept(p, ",") && !tok_is(p, p->pos, "}")) {
          fail(p, "expected ',' in static array initializer");
          return false;
        }
        continue;
      }
      if (*element_index >= element_count) {
        fail(p, "too many values in static array initializer");
        return false;
      }
      if (c_type_is_pointer(type) || type == C_FUNC_PTR) {
        size_t range_begin = range_first * width;
        size_t range_end = range_last * width;
        for (size_t relocation = 0; relocation < data->reloc_count;) {
          c_global_reloc_t *existing = &data->relocs[relocation];
          if (existing->offset < range_begin || existing->offset >= range_end) {
            ++relocation;
            continue;
          }
          free(existing->owned_target);
          memmove(existing, existing + 1,
                  (data->reloc_count - relocation - 1) * sizeof(*existing));
          --data->reloc_count;
        }
        size_t reloc_start = data->reloc_count;
        if (!initialize_data_pointer(p, data, range_first * width, bytes))
          return false;
        size_t reloc_count = data->reloc_count - reloc_start;
        size_t copies = range_last - range_first - 1;
        if (copies && reloc_count &&
            copies > (SIZE_MAX - data->reloc_count) / reloc_count) {
          fail(p, "static pointer-array range relocation count overflows");
          return false;
        }
        size_t required = data->reloc_count + copies * reloc_count;
        if (required > data->reloc_capacity) {
          if (required > SIZE_MAX / sizeof(*data->relocs)) {
            fail(p, "static pointer-array relocation table is too large");
            return false;
          }
          c_global_reloc_t *grown =
              realloc(data->relocs, required * sizeof(*data->relocs));
          if (!grown) {
            fail(p, "out of memory growing static pointer-array relocations");
            return false;
          }
          data->relocs = grown;
          data->reloc_capacity = required;
        }
        for (size_t index = range_first + 1; index < range_last; ++index) {
          size_t offset = index * width;
          memcpy(bytes + offset, bytes + range_first * width, width);
          for (size_t relocation = 0; relocation < reloc_count; ++relocation) {
            c_global_reloc_t copy = data->relocs[reloc_start + relocation];
            copy.offset += offset - range_first * width;
            if (copy.owned_target) {
              copy.owned_target = strdup(copy.owned_target);
              if (!copy.owned_target) {
                fail(p,
                     "out of memory copying static pointer-array relocation");
                return false;
              }
              copy.target.start = copy.owned_target;
            }
            data->relocs[data->reloc_count++] = copy;
          }
        }
        *element_index = range_last;
        if (!accept(p, ",") && !tok_is(p, p->pos, "}")) {
          fail(p, "expected ',' in static array initializer");
          return false;
        }
        continue;
      }
      int64_t value = 0;
      if (!parse_global_const_expr(p, &p->pos, &value)) {
        fail(p, "static scalar arrays require constant integer elements");
        return false;
      }
      if (*element_index >= element_count) {
        fail(p, "too many values in static array initializer");
        return false;
      }
      uint64_t bits = (uint64_t)value;
      for (size_t index = range_first; index < range_last; ++index)
        for (size_t byte = 0; byte < width; ++byte)
          bytes[index * width + byte] = (unsigned char)(bits >> (byte * 8));
      *element_index = range_last;
    }
    if (!accept(p, ",") && !tok_is(p, p->pos, "}")) {
      fail(p, "expected ',' in static array initializer");
      return false;
    }
  }
  return expect(p, "}");
}

static bool parse_static_aggregate_array_initializer(
    c_parser_t *p, c_global_t *data, c_agg_type_t *aggregate,
    unsigned char *bytes, size_t element_count, size_t *element_index) {
  if (!expect(p, "{"))
    return false;
  while (!tok_is(p, p->pos, "}")) {
    if (accept(p, "[")) {
      int64_t designator = 0;
      if (!parse_global_const_expr(p, &p->pos, &designator) || designator < 0 ||
          !expect(p, "]") || !expect(p, "="))
        return false;
      *element_index = (size_t)designator;
    }
    if (*element_index >= element_count) {
      fail(p, "static aggregate array initializer exceeds its declared bound");
      return false;
    }
    if (!init_aggregate(p, data, aggregate, bytes,
                        *element_index * aggregate->size, 0, -1))
      return false;
    ++*element_index;
    if (!accept(p, ",") && !tok_is(p, p->pos, "}")) {
      fail(p, "expected ',' or '}' in static aggregate array initializer");
      return false;
    }
  }
  return expect(p, "}");
}

static bool parse_static_array_compound_literal_size(c_parser_t *p,
                                                     uint64_t *size,
                                                     bool *matched) {
  *matched = false;
  c_parser_t probe = *p;
  char probe_error[128] = {0};
  probe.err = probe_error;
  probe.err_len = sizeof(probe_error);
  probe.type_lookahead = true;
  size_t grouping_parentheses = 0;
  while (tok_is(&probe, probe.pos + grouping_parentheses, "(") &&
         tok_is(&probe, probe.pos + grouping_parentheses + 1, "("))
    ++grouping_parentheses;
  probe.pos += grouping_parentheses + 1;
  c_type_t element_type;
  if (!parse_type(&probe, &element_type, false))
    return true;
  c_agg_type_t *aggregate = element_type == C_AGG ? probe.last_type_agg : NULL;
  if ((!c_type_is_integer(element_type) && !c_type_is_pointer(element_type)) &&
      (!aggregate || !aggregate->complete))
    return true;
  size_t extents[C_MAX_ARRAY_RANK] = {0};
  unsigned rank = 0;
  bool incomplete_outer = false;
  size_t inner_count = 1;
  while (accept(&probe, "[")) {
    if (rank == C_MAX_ARRAY_RANK)
      return true;
    if (rank == 0 && accept(&probe, "]")) {
      incomplete_outer = true;
      ++rank;
      continue;
    }
    uint64_t extent = 0;
    if (!parse_array_extent_expr(&probe, 1, &extent) || !extent ||
        extent > 65536 || !expect(&probe, "]") ||
        inner_count > 65536 / (size_t)extent)
      return true;
    extents[rank++] = (size_t)extent;
    if (rank > 1)
      inner_count *= (size_t)extent;
  }
  if (!rank || !expect(&probe, ")") || !tok_is(&probe, probe.pos, "{"))
    return true;

  *matched = true;
  size_t outer_extent = extents[0];
  bool empty_initializer =
      tok_is(&probe, probe.pos, "{") && tok_is(&probe, probe.pos + 1, "}");
  if (incomplete_outer) {
    outer_extent = count_global_aggregate_array_initializer(&probe, probe.pos);
    if ((!outer_extent && !empty_initializer) ||
        outer_extent > 65536 / inner_count)
      return false;
  }
  if ((!outer_extent && !empty_initializer) ||
      outer_extent > 65536 / inner_count)
    return false;
  size_t count = outer_extent * inner_count;
  size_t element_size =
      aggregate ? aggregate->size : c_type_size(p->abi, element_type);
  if (!element_size || count > UINT64_MAX / element_size)
    return false;
  uint64_t result = (uint64_t)count * element_size;
  size_t after_initializer = skip_balanced(&probe, probe.pos, "{", "}");
  if (after_initializer >= probe.token_count)
    return false;
  probe.pos = after_initializer;
  while (grouping_parentheses) {
    if (!expect(&probe, ")"))
      return false;
    --grouping_parentheses;
  }
  bool indexed_element = false;
  if (accept(&probe, "[")) {
    int64_t index = 0;
    if (!parse_global_const_expr(&probe, &probe.pos, &index) || index < 0 ||
        !expect(&probe, "]"))
      return false;
    indexed_element = true;
  }
  p->pos = probe.pos;
  *size = indexed_element ? (uint64_t)inner_count * element_size : result;
  return true;
}

static bool try_initialize_static_array_compound_literal(c_parser_t *p,
                                                         c_global_t *data,
                                                         size_t offset,
                                                         bool *matched) {
  *matched = false;
  size_t wrapper_parentheses = 0;
  size_t literal_start = p->pos;
  while (tok_is(p, literal_start, "(") && (tok_is(p, literal_start + 1, "(") ||
                                           tok_is(p, literal_start + 1, "&"))) {
    ++wrapper_parentheses;
    ++literal_start;
  }
  bool address_of = tok_is(p, literal_start, "&");
  if (address_of)
    ++literal_start;
  if (!tok_is(p, literal_start, "("))
    return true;

  c_parser_t probe = *p;
  char probe_error[128] = {0};
  probe.err = probe_error;
  probe.err_len = sizeof(probe_error);
  probe.type_lookahead = true;
  size_t grouping_parentheses = 0;
  while (tok_is(&probe, literal_start + grouping_parentheses, "(") &&
         tok_is(&probe, literal_start + grouping_parentheses + 1, "("))
    ++grouping_parentheses;
  probe.pos = literal_start + grouping_parentheses + 1;
  c_type_t element_type;
  if (!parse_type(&probe, &element_type, false))
    return true;
  c_agg_type_t *element_aggregate =
      element_type == C_AGG ? probe.last_type_agg : NULL;
  if ((!c_type_is_integer(element_type) && !c_type_is_pointer(element_type)) &&
      (!element_aggregate || !element_aggregate->complete))
    return true;

  bool is_const = probe.last_type_const;
  size_t alignment = probe.last_type_alignment;
  size_t extents[C_MAX_ARRAY_RANK] = {0};
  unsigned rank = 0;
  bool incomplete_outer = false;
  size_t inner_count = 1;
  while (accept(&probe, "[")) {
    if (rank == C_MAX_ARRAY_RANK) {
      fail(p, "static array compound literal exceeds the array-rank limit");
      return false;
    }
    if (rank == 0 && accept(&probe, "]")) {
      incomplete_outer = true;
      ++rank;
      continue;
    }
    uint64_t extent = 0;
    if (!parse_array_extent_expr(&probe, 1, &extent) || !extent ||
        extent > 65536 || !expect(&probe, "]") ||
        inner_count > 65536 / (size_t)extent) {
      fail(p, "static array compound literal has an invalid extent");
      return false;
    }
    extents[rank++] = (size_t)extent;
    if (rank > 1)
      inner_count *= (size_t)extent;
  }
  if (!rank || !expect(&probe, ")") || !tok_is(&probe, probe.pos, "{"))
    return true;
  *matched = true;

  size_t outer_extent = extents[0];
  bool empty_initializer =
      tok_is(&probe, probe.pos, "{") && tok_is(&probe, probe.pos + 1, "}");
  if (incomplete_outer) {
    outer_extent = count_global_aggregate_array_initializer(&probe, probe.pos);
    if ((!outer_extent && !empty_initializer) ||
        outer_extent > 65536 / inner_count) {
      fail(p, "cannot infer static array compound literal extent");
      return false;
    }
    extents[0] = outer_extent;
  }
  if ((!outer_extent && !empty_initializer) ||
      outer_extent > 65536 / inner_count) {
    fail(p, "static array compound literal exceeds frontend limits");
    return false;
  }
  size_t element_count = outer_extent * inner_count;
  if (element_aggregate && rank != 1) {
    fail(p,
         "multidimensional static aggregate compound literals are unsupported");
    return false;
  }
  size_t element_size = element_aggregate ? element_aggregate->size
                                          : c_type_size(p->abi, element_type);
  if (!element_size || element_count > SIZE_MAX / element_size ||
      p->global_count >= p->global_capacity) {
    fail(p, "static array compound literal has an unsupported size");
    return false;
  }
  size_t byte_count = element_count * element_size;
  unsigned char *storage = calloc(byte_count ? byte_count : 1, 1);
  char generated_name[64];
  int name_len = snprintf(generated_name, sizeof(generated_name),
                          "__nyc_compound_array_%016llx_%zu",
                          (unsigned long long)c_source_hash(p->base_path),
                          p->global_count);
  char *owned_name = name_len > 0 && (size_t)name_len < sizeof(generated_name)
                         ? strdup(generated_name)
                         : NULL;
  if (!storage || !owned_name) {
    free(storage);
    free(owned_name);
    fail(p, "out of memory allocating static array compound literal");
    return false;
  }
  c_global_t global = {.name = {.kind = NY_CTOK_IDENT,
                                .start = owned_name,
                                .len = (size_t)name_len},
                       .bytes = storage,
                       .owned_name = owned_name,
                       .len = byte_count,
                       .type = element_type,
                       .agg = element_aggregate,
                       .is_array = true,
                       .array_rank = rank,
                       .array_stride = inner_count * element_size,
                       .array_shape = {.rank = rank,
                                       .element_type = element_type,
                                       .element_size = element_size},
                       .is_pointer_array = c_type_is_pointer(element_type),
                       .is_function_pointer_array = element_type == C_FUNC_PTR,
                       .owner_function =
                           p->function ? p->function->name : (ny_ctok_t){0},
                       .is_const = is_const,
                       .alignment = alignment};
  memcpy(global.array_shape.extent, extents, rank * sizeof(*extents));
  p->pos = probe.pos;
  size_t element_index = 0;
  bool initialized = element_aggregate
                         ? parse_static_aggregate_array_initializer(
                               p, &global, element_aggregate, storage,
                               element_count, &element_index)
                         : parse_static_array_initializer(
                               p, element_type, &global, storage, element_count,
                               &element_index, 0, true, inner_count);
  if (!initialized) {
    for (size_t i = 0; i < global.reloc_count; ++i)
      free(global.relocs[i].owned_target);
    free(global.relocs);
    free(storage);
    free(owned_name);
    return false;
  }
  while (grouping_parentheses) {
    if (!expect(p, ")")) {
      for (size_t i = 0; i < global.reloc_count; ++i)
        free(global.relocs[i].owned_target);
      free(global.relocs);
      free(storage);
      free(owned_name);
      return false;
    }
    --grouping_parentheses;
  }
  int64_t addend = 0;
  const c_agg_type_t *selected_aggregate = element_aggregate;
  size_t selected_stride = element_size;
  bool selected_array_element = false;
  if (address_of) {
    c_parser_t selector = *p;
    while (tok_is(&selector, selector.pos, "[") ||
           tok_is(&selector, selector.pos, ".")) {
      if (accept(&selector, "[")) {
        int64_t index = 0;
        int64_t displacement = 0;
        size_t extent = selected_array_element ? 0 : element_count;
        if (!selected_stride ||
            !parse_global_const_expr(&selector, &selector.pos, &index) ||
            index < 0 || (extent && (uint64_t)index >= extent) ||
            !expect(&selector, "]") ||
            __builtin_mul_overflow(index, (int64_t)selected_stride,
                                   &displacement) ||
            __builtin_add_overflow(addend, displacement, &addend)) {
          fail(p, "invalid static array compound-literal subscript");
          for (size_t i = 0; i < global.reloc_count; ++i)
            free(global.relocs[i].owned_target);
          free(global.relocs);
          free(storage);
          free(owned_name);
          return false;
        }
        selected_stride = 0;
        selected_array_element = true;
      } else {
        ++selector.pos;
        ny_ctok_t member;
        const c_agg_field_t *field = NULL;
        int64_t field_offset = 0;
        if (!selected_aggregate || !parse_name(&selector, &member) ||
            !(field = find_field(selected_aggregate, member)) ||
            field->offset > INT64_MAX ||
            __builtin_add_overflow(addend, (int64_t)field->offset,
                                   &field_offset)) {
          fail(p, "invalid static array compound-literal member address");
          for (size_t i = 0; i < global.reloc_count; ++i)
            free(global.relocs[i].owned_target);
          free(global.relocs);
          free(storage);
          free(owned_name);
          return false;
        }
        addend = field_offset;
        selected_aggregate = field->agg;
        selected_stride =
            field->kind == C_AGG_FIELD_ARRAY ? field->elem_size : 0;
        selected_array_element = field->kind == C_AGG_FIELD_ARRAY;
      }
    }
    p->pos = selector.pos;
  }
  while (wrapper_parentheses) {
    if (!expect(p, ")")) {
      for (size_t i = 0; i < global.reloc_count; ++i)
        free(global.relocs[i].owned_target);
      free(global.relocs);
      free(storage);
      free(owned_name);
      return false;
    }
    --wrapper_parentheses;
  }
  if (p->global_count >= p->global_capacity) {
    for (size_t i = 0; i < global.reloc_count; ++i)
      free(global.relocs[i].owned_target);
    free(global.relocs);
    free(storage);
    free(owned_name);
    fail(p, "C frontend global data table capacity exceeded");
    return false;
  }

  p->globals[p->global_count++] = global;
  if (!append_global_reloc(p, data, offset, global.name)) {
    --p->global_count;
    for (size_t i = 0; i < global.reloc_count; ++i)
      free(global.relocs[i].owned_target);
    free(global.relocs);
    free(storage);
    free(owned_name);
    return false;
  }
  data->relocs[data->reloc_count - 1].addend = addend;
  return true;
}

static bool try_initialize_static_aggregate_compound_literal(c_parser_t *p,
                                                             c_global_t *data,
                                                             size_t offset,
                                                             bool *matched) {
  *matched = false;
  size_t wrapper_parentheses = 0;
  size_t literal_start = p->pos;
  while (tok_is(p, literal_start, "(") && (tok_is(p, literal_start + 1, "(") ||
                                           tok_is(p, literal_start + 1, "&"))) {
    ++wrapper_parentheses;
    ++literal_start;
  }
  if (!tok_is(p, literal_start, "&") || !tok_is(p, literal_start + 1, "("))
    return true;

  c_parser_t probe = *p;
  char probe_error[128] = {0};
  probe.err = probe_error;
  probe.err_len = sizeof(probe_error);
  probe.type_lookahead = true;
  probe.pos = literal_start + 2;
  c_type_t type;
  if (!parse_type(&probe, &type, false) || type != C_AGG ||
      !probe.last_type_agg || !probe.last_type_agg->complete ||
      !expect(&probe, ")") || !tok_is(&probe, probe.pos, "{"))
    return true;

  *matched = true;
  c_agg_type_t *aggregate = probe.last_type_agg;
  bool is_const = probe.last_type_const;
  if (!aggregate->size || aggregate->size > 65536 ||
      p->global_count >= p->global_capacity) {
    fail(p, "static aggregate compound literal has an unsupported size");
    return false;
  }
  unsigned char *storage = calloc(aggregate->size, 1);
  char generated_name[64];
  int name_len = snprintf(generated_name, sizeof(generated_name),
                          "__nyc_compound_aggregate_%016llx_%zu",
                          (unsigned long long)c_source_hash(p->base_path),
                          p->global_count);
  char *owned_name = name_len > 0 && (size_t)name_len < sizeof(generated_name)
                         ? strdup(generated_name)
                         : NULL;
  if (!storage || !owned_name) {
    free(storage);
    free(owned_name);
    fail(p, "out of memory allocating static aggregate compound literal");
    return false;
  }
  c_global_t global = {.name = {.kind = NY_CTOK_IDENT,
                                .start = owned_name,
                                .len = (size_t)name_len},
                       .bytes = storage,
                       .owned_name = owned_name,
                       .len = aggregate->size,
                       .type = C_AGG,
                       .agg = aggregate,
                       .owner_function =
                           p->function ? p->function->name : (ny_ctok_t){0},
                       .is_const = is_const};
  p->pos = probe.pos;
  if (!init_aggregate(p, &global, aggregate, storage, 0, 0, -1)) {
    for (size_t i = 0; i < global.reloc_count; ++i)
      free(global.relocs[i].owned_target);
    free(global.relocs);
    free(storage);
    free(owned_name);
    return false;
  }
  for (size_t i = 0; i < wrapper_parentheses; ++i) {
    if (!expect(p, ")")) {
      for (size_t relocation = 0; relocation < global.reloc_count; ++relocation)
        free(global.relocs[relocation].owned_target);
      free(global.relocs);
      free(storage);
      free(owned_name);
      return false;
    }
  }
  if (p->global_count >= p->global_capacity) {
    for (size_t i = 0; i < global.reloc_count; ++i)
      free(global.relocs[i].owned_target);
    free(global.relocs);
    free(storage);
    free(owned_name);
    fail(p, "C frontend global data table capacity exceeded");
    return false;
  }
  p->globals[p->global_count++] = global;
  if (!append_global_reloc(p, data, offset, global.name)) {
    --p->global_count;
    for (size_t i = 0; i < global.reloc_count; ++i)
      free(global.relocs[i].owned_target);
    free(global.relocs);
    free(storage);
    free(owned_name);
    return false;
  }
  return true;
}

static bool lower_static_scalar_declaration(c_parser_t *p, c_type_t type,
                                            ny_ctok_t name, bool is_const,
                                            bool is_volatile,
                                            c_agg_type_t *aggregate,
                                            ny_ctok_t section) {
  if (!c_type_is_integer(type) && !c_type_is_pointer(type) &&
      type != C_FUNC_PTR && tok_is(p, p->pos, "[")) {
    fail(p, "static scalar arrays require integer or pointer elements");
    return false;
  }
  size_t extents[C_MAX_ARRAY_RANK] = {0};
  unsigned rank = 0;
  size_t count = 1;
  bool inferred_outer = false;
  while (accept(p, "[")) {
    if (rank == 0 && accept(p, "]")) {
      inferred_outer = true;
      ++rank;
      continue;
    }
    uint64_t extent = 0;
    if (rank == C_MAX_ARRAY_RANK || !parse_array_extent_expr(p, 1, &extent) ||
        !extent || extent > 65536 || !expect(p, "]") ||
        count > 65536 / (size_t)extent) {
      char message[192];
      snprintf(message, sizeof(message),
               "static scalar array `%.*s`: invalid or unsupported dimension "
               "%u",
               (int)(name.len > 80 ? 80 : name.len), name.start, rank + 1);
      fail(p, message);
      return false;
    }
    extents[rank++] = (size_t)extent;
    count *= (size_t)extent;
  }
  if (type == C_FUNC_PTR && rank && tok_is(p, p->pos, ")")) {
    if (!expect(p, ")")) {
      fail(p, "malformed static local function-pointer array type");
      return false;
    }
    if (accept(p, "(")) {
      size_t parameters_after = skip_balanced(p, p->pos - 1, "(", ")");
      if (parameters_after >= p->token_count) {
        fail(p, "unterminated static local function-pointer array type");
        return false;
      }
      p->pos = parameters_after;
    }
  }
  ny_ctok_t trailing_section = {0};
  ny_ctok_t trailing_cleanup = {0};
  if (!parse_trailing_local_attributes(p, &trailing_section, &trailing_cleanup))
    return false;
  if (trailing_cleanup.len || (section.len && trailing_section.len)) {
    fail(p, "invalid attributes on static local object");
    return false;
  }
  if (trailing_section.len)
    section = trailing_section;
  bool has_initializer = accept(p, "=");
  unsigned char *string_initializer = NULL;
  size_t string_initializer_len = 0;
  bool wide_string_initializer =
      has_initializer && rank && (type == C_I16 || type == C_U16) &&
      p->pos < p->token_count && p->tokens[p->pos].kind == NY_CTOK_STRING &&
      p->tokens[p->pos].len >= 2 && p->tokens[p->pos].start[0] == 'L' &&
      p->tokens[p->pos].start[1] == '"';
  bool has_string_initializer =
      has_initializer && rank &&
      ((type == C_CHAR || type == C_U8) || wide_string_initializer) &&
      p->pos < p->token_count && p->tokens[p->pos].kind == NY_CTOK_STRING &&
      (wide_string_initializer || p->tokens[p->pos].start[0] != 'L');
  if (has_string_initializer) {
    size_t string_unit_size = wide_string_initializer ? sizeof(uint16_t) : 1;
    while (p->pos < p->token_count &&
           p->tokens[p->pos].kind == NY_CTOK_STRING) {
      unsigned char *part = NULL;
      size_t part_len = 0;
      ny_ctok_t token = p->tokens[p->pos++];
      bool part_is_wide =
          token.len >= 2 && token.start[0] == 'L' && token.start[1] == '"';
      if (part_is_wide != wide_string_initializer ||
          !(wide_string_initializer
                ? decode_wide_string_token(token, &part, &part_len)
                : decode_string_token(token, &part, &part_len)) ||
          part_len < string_unit_size || part_len % string_unit_size != 0) {
        free(part);
        free(string_initializer);
        fail(p, "invalid static character-array string initializer");
        return false;
      }
      size_t old_content = string_initializer_len
                               ? string_initializer_len - string_unit_size
                               : 0;
      size_t part_content = part_len - string_unit_size;
      if (old_content > 65536 || part_content > 65536 - old_content) {
        free(part);
        free(string_initializer);
        fail(p, "static character-array initializer exceeds frontend limit");
        return false;
      }
      unsigned char *joined =
          realloc(string_initializer, old_content + part_len);
      if (!joined) {
        free(part);
        free(string_initializer);
        fail(p, "out of memory decoding static character-array initializer");
        return false;
      }
      string_initializer = joined;
      memcpy(string_initializer + old_content, part, part_len);
      string_initializer_len = old_content + part_len;
      free(part);
    }
    if (inferred_outer) {
      count = string_initializer_len / string_unit_size;
      extents[0] = count;
    } else if (string_initializer_len > count * string_unit_size &&
               string_initializer_len - string_unit_size >
                   count * string_unit_size) {
      free(string_initializer);
      fail(p, "string initializer does not fit static character array");
      return false;
    }
  }
  if (inferred_outer) {
    if (!has_string_initializer) {
      size_t outer_count =
          has_initializer ? count_global_aggregate_array_initializer(p, p->pos)
                          : 0;
      if (!outer_count || count > 65536 / outer_count) {
        fail(p, "unsized static scalar array requires a bounded initializer");
        return false;
      }
      extents[0] = outer_count;
      count *= outer_count;
    }
  }
  size_t width = c_type_size(p->abi, type);
  if (count > SIZE_MAX / width || p->global_count >= p->global_capacity ||
      p->local_count >= p->local_capacity || p->next_local == SIZE_MAX) {
    free(string_initializer);
    fail(p, "static scalar array exceeds frontend capacity");
    return false;
  }
  size_t byte_count = count * width;
  unsigned char *bytes = calloc(byte_count ? byte_count : 1, 1);
  char generated_name[48];
  int generated_len = snprintf(generated_name, sizeof(generated_name),
                               "__nyc_static_%zu", p->global_count);
  char *owned_name =
      generated_len > 0 && (size_t)generated_len < sizeof(generated_name)
          ? strdup(generated_name)
          : NULL;
  if (!bytes || !owned_name) {
    free(bytes);
    free(owned_name);
    free(string_initializer);
    fail(p, "out of memory allocating static scalar array");
    return false;
  }
  c_global_t global = {.name = {.kind = NY_CTOK_IDENT,
                                .start = owned_name,
                                .len = (size_t)generated_len},
                       .bytes = bytes,
                       .owned_name = owned_name,
                       .len = byte_count,
                       .type = type,
                       .agg = aggregate,
                       .is_array = rank != 0,
                       .is_pointer_array = rank && c_type_is_pointer(type),
                       .is_function_pointer_array = rank && type == C_FUNC_PTR,
                       .owner_function =
                           p->function ? p->function->name : (ny_ctok_t){0},
                       .is_const = is_const};
  if (has_initializer) {
    if (!rank) {
      if (c_type_is_pointer(type)) {
        if (!initialize_data_pointer(p, &global, 0, bytes)) {
          free(bytes);
          free(owned_name);
          free(string_initializer);
          free(global.relocs);
          return false;
        }
      } else if (c_type_is_float(type)) {
        double value = 0;
        if (!parse_global_float_add(p, &p->pos, &value)) {
          free(bytes);
          free(owned_name);
          fail(p,
               "static floating-point object requires a constant initializer");
          return false;
        }
        if (type == C_F32) {
          float narrow = (float)value;
          memcpy(bytes, &narrow, sizeof(narrow));
        } else {
          memcpy(bytes, &value, sizeof(value));
        }
      } else {
        int64_t value = 0;
        if (!parse_global_const_expr(p, &p->pos, &value)) {
          free(bytes);
          free(owned_name);
          fail(p, "static scalar object requires a constant initializer");
          return false;
        }
        memcpy(bytes, &value, width);
      }
    } else if (has_string_initializer) {
      size_t capacity = count * width;
      size_t copy_len =
          string_initializer_len < capacity ? string_initializer_len : capacity;
      memcpy(bytes, string_initializer, copy_len);
      free(string_initializer);
    } else {
      size_t element_index = 0;
      if (!parse_static_array_initializer(p, type, &global, bytes, count,
                                          &element_index, 0, rank != 0,
                                          rank > 1 ? count / extents[0] : 1)) {
        free(bytes);
        free(owned_name);
        free(string_initializer);
        free(global.relocs);
        return false;
      }
    }
  }
  bool more_declarators = accept(p, ",");
  if (!more_declarators && !expect(p, ";")) {
    free(bytes);
    free(owned_name);
    free(string_initializer);
    free(global.relocs);
    return false;
  }
  if (section.len) {
    char section_name[64];
    if (!copy_quoted_token(p, section, section_name, sizeof(section_name)) ||
        !(global.section = strdup(section_name))) {
      free(bytes);
      free(owned_name);
      free(global.relocs);
      fail(p, "invalid or unavailable static local section name");
      return false;
    }
  }
  p->globals[p->global_count++] = global;
  c_local_t *local = &p->locals[p->local_count++];
  *local = (c_local_t){.name = name,
                       .static_name = global.name,
                       .type = type,
                       .agg = aggregate,
                       .slot = p->next_local++,
                       .is_const = is_const,
                       .is_volatile = is_volatile,
                       .volatile_address = -1,
                       .array_count = rank ? count : 0,
                       .array_rank = rank};
  memcpy(local->array_extent, extents, sizeof(extents));
  if (more_declarators) {
    ny_ctok_t next_name;
    if (!parse_name(p, &next_name))
      return false;
    return lower_static_scalar_declaration(p, type, next_name, is_const,
                                           is_volatile, aggregate, section);
  }
  return true;
}

/*
 * Declare one struct/union local (optionally an array of them). Storage
 * lives in the entry-block aggregate area; uninitialized declarations are
 * zero-filled so reads are deterministic.
 */
static bool literal_aggregate_initializer(c_parser_t *p) {
  size_t end = skip_balanced(p, p->pos, "{", "}");
  for (size_t i = p->pos; i < end; ++i)
    if (p->tokens[i].kind == NY_CTOK_IDENT)
      return false;
  return true;
}

static bool zero_aggregate_initializer(const c_parser_t *p) {
  return tok_is(p, p->pos, "{") && tok_is(p, p->pos + 1, "0") &&
         tok_is(p, p->pos + 2, "}");
}

static bool parse_local_aggregate_array_designator(c_parser_t *p, size_t count,
                                                   size_t *first, size_t *end) {
  int64_t first_index = -1;
  int64_t last_index = -1;
  if (!parse_global_const_expr(p, &p->pos, &first_index) || first_index < 0) {
    fail(p, "invalid local aggregate array designator");
    return false;
  }
  last_index = first_index;
  if (accept(p, "...") && (!parse_global_const_expr(p, &p->pos, &last_index) ||
                           last_index < first_index)) {
    fail(p, "invalid local aggregate array range designator");
    return false;
  }
  if (!expect(p, "]") || !expect(p, "=") || (uint64_t)last_index >= count) {
    fail(p, "local aggregate array designator is outside its declared bound");
    return false;
  }
  *first = (size_t)first_index;
  *end = (size_t)last_index + 1;
  return true;
}

static bool copy_local_aggregate_initializer_range(c_parser_t *p, int base,
                                                   size_t element_size,
                                                   size_t first, size_t end) {
  int source = lower_add_const(p, base, first * element_size);
  if (source < 0)
    return false;
  for (size_t element = first + 1; element < end; ++element) {
    int destination = lower_add_const(p, base, element * element_size);
    if (destination < 0 ||
        !lower_copy_struct(p, destination, source, element_size))
      return false;
  }
  return true;
}

static bool copy_local_aggregate_data_range(c_parser_t *p, c_global_t *data,
                                            size_t element_size, size_t first,
                                            size_t end,
                                            size_t source_reloc_start) {
  if (!element_size || source_reloc_start > data->reloc_count) {
    fail(p, "invalid local aggregate range initializer state");
    return false;
  }
  size_t source_offset = first * element_size;
  size_t source_reloc_count = data->reloc_count - source_reloc_start;
  size_t copies = end - first - 1;
  if (source_reloc_count &&
      copies > (SIZE_MAX - data->reloc_count) / source_reloc_count) {
    fail(p, "local aggregate range relocation count overflows");
    return false;
  }
  size_t required = data->reloc_count + copies * source_reloc_count;
  if (required > data->reloc_capacity) {
    if (required > SIZE_MAX / sizeof(*data->relocs)) {
      fail(p, "local aggregate range relocation storage overflows");
      return false;
    }
    c_global_reloc_t *relocs =
        realloc(data->relocs, required * sizeof(*data->relocs));
    if (!relocs) {
      fail(p, "out of memory copying local aggregate range relocations");
      return false;
    }
    data->relocs = relocs;
    data->reloc_capacity = required;
  }
  for (size_t element = first + 1; element < end; ++element) {
    size_t destination_offset = element * element_size;
    memcpy(data->bytes + destination_offset, data->bytes + source_offset,
           element_size);
    for (size_t relocation = 0; relocation < source_reloc_count; ++relocation) {
      c_global_reloc_t copy = data->relocs[source_reloc_start + relocation];
      if (copy.offset < source_offset ||
          copy.offset - source_offset >= element_size ||
          destination_offset > SIZE_MAX - (copy.offset - source_offset)) {
        fail(p, "local aggregate range relocation is outside its element");
        return false;
      }
      copy.offset = destination_offset + (copy.offset - source_offset);
      if (copy.owned_target) {
        copy.owned_target = strdup(copy.owned_target);
        if (!copy.owned_target) {
          fail(p, "out of memory copying local aggregate range relocation");
          return false;
        }
      }
      data->relocs[data->reloc_count++] = copy;
    }
  }
  return true;
}

static bool lower_aggregate_declaration(c_parser_t *p, bool semicolon) {
  bool is_static = accept(p, "static");
  size_t declaration_type_start = p->pos;
  c_type_t type;
  if (!parse_type(p, &type, false))
    return false;
  while (tok_is_attribute(p, p->pos))
    if (!parse_function_type_attributes(p))
      return false;
  bool aggregate_pointer =
      p->last_type_base_valid && p->last_type_base_type == C_AGG;
  if (aggregate_pointer && p->last_type_pointer_count > 2)
    type = C_OPAQUE_PTR_PTR_PTR;
  if (type != C_AGG && type != C_AGG_PTR && type != C_AGG_PTR_PTR &&
      type != C_OPAQUE_PTR &&
      !(type == C_OPAQUE_PTR_PTR_PTR && aggregate_pointer)) {
    fail(p, "aggregate declaration requires a struct or union type");
    return false;
  }
  if (type == C_AGG && semicolon && accept(p, ";"))
    return true;
  c_agg_type_t *decl_agg = p->last_type_agg;
  bool typedef_array = type == C_AGG && p->last_type_is_array;
  size_t typedef_array_count = 0;
  if (typedef_array && decl_agg && decl_agg->size &&
      p->last_type_array_size % decl_agg->size == 0)
    typedef_array_count = p->last_type_array_size / decl_agg->size;
  unsigned declared_stars = 0;
  for (size_t i = declaration_type_start; i < p->pos; ++i)
    declared_stars += tok_is(p, i, "*");
  c_type_t declarator_base_type =
      aggregate_pointer ? C_AGG : c_remove_pointer_levels(type, declared_stars);
  c_type_t base_type = aggregate_pointer ? C_AGG : type;
  while (base_type != C_AGG && c_type_is_pointer(base_type)) {
    c_type_t pointee = c_remove_pointer_levels(base_type, 1);
    if (pointee == C_BAD)
      break;
    base_type = pointee;
  }
  if (base_type != C_AGG) {
    fail(p, "aggregate declaration has an unsupported pointer base type");
    return false;
  }
  bool is_const =
      c_type_is_pointer(type) ? p->last_type_const_pointer : p->last_type_const;
  bool ok = true;
  bool first_declarator = true;
  do {
    if (!first_declarator) {
      unsigned stars = 0;
      while (accept(p, "*")) {
        ++stars;
        while (accept(p, "const") || accept(p, "volatile") ||
               accept(p, "restrict") || accept(p, "__restrict") ||
               accept(p, "__restrict__")) {
        }
      }
      type = c_add_pointer_levels(declarator_base_type, stars);
      if (type == C_BAD) {
        fail(p, "unsupported aggregate pointer declarator");
        ok = false;
        break;
      }
    }
    first_declarator = false;
    ny_ctok_t name;
    if (!parse_name(p, &name)) {
      ok = false;
      break;
    }
    size_t local_alignment = p->last_type_alignment;
    ny_ctok_t local_mode = p->last_type_mode;
    ny_ctok_t local_section = p->last_type_section;
    p->last_type_section = (ny_ctok_t){0};
    ny_ctok_t local_cleanup = {0};
    if (!parse_trailing_local_attributes(p, &local_section, &local_cleanup)) {
      ok = false;
      break;
    }
    if (is_static && local_cleanup.len) {
      fail(p, "cleanup attribute requires an automatic local object");
      ok = false;
      break;
    }
    if ((!is_static && local_section.len) ||
        p->last_type_alignment != local_alignment ||
        !tok_same(p->last_type_mode, local_mode)) {
      if (!getenv("NYC_KERNEL_COMPAT")) {
        fail(p, "placement and type attributes on local declarations are "
                "unsupported");
        ok = false;
        break;
      }
      p->last_type_alignment = local_alignment;
      p->last_type_mode = local_mode;
      if (!is_static)
        local_section = (ny_ctok_t){0};
    }
    size_t count = typedef_array ? typedef_array_count : 1;
    bool is_array = typedef_array;
    if (is_array && !count) {
      fail(p, "local aggregate array typedef requires a complete extent");
      ok = false;
      break;
    }
    if ((type == C_AGG || c_type_is_pointer(type)) && accept(p, "[")) {
      is_array = true;
      if (accept(p, "]")) {
        count = 0;
      } else {
        uint64_t parsed = 0;
        if (!parse_array_extent_expr(p, 0, &parsed) || !expect(p, "]")) {
          fail(p, "array declarations require a constant extent");
          ok = false;
          break;
        }
        if (parsed == 0 || parsed > 65536) {
          fail(p, "array extent must be between 1 and 65536");
          ok = false;
          break;
        }
        count = (size_t)parsed;
      }
    }
    {
      ny_ctok_t trailing_section_after = {0};
      ny_ctok_t trailing_cleanup_after = {0};
      if (!parse_trailing_local_attributes(p, &trailing_section_after,
                                           &trailing_cleanup_after)) {
        ok = false;
        break;
      }
      if (trailing_section_after.len)
        local_section = trailing_section_after;
      if (is_static && trailing_cleanup_after.len) {
        fail(p, "cleanup attribute requires an automatic local object");
        ok = false;
        break;
      }
    }
    if (is_static) {
      if (type != C_AGG || !decl_agg || !decl_agg->complete ||
          p->global_count >= p->global_capacity) {
        fail(p, "static C aggregate requires a complete aggregate type");
        ok = false;
        break;
      }
      bool has_initializer = accept(p, "=");
      if (is_array && has_initializer && count == 0 && tok_is(p, p->pos, "{"))
        count = count_global_aggregate_array_initializer(p, p->pos);
      if (!count || decl_agg->size > SIZE_MAX / count) {
        fail(p, "static C aggregate has an invalid object size");
        ok = false;
        break;
      }
      size_t total_size = decl_agg->size * count;
      unsigned char *bytes = calloc(total_size ? total_size : 1, 1);
      char generated_name[48];
      int generated_len = snprintf(generated_name, sizeof(generated_name),
                                   "__nyc_static_%zu", p->global_count);
      char *owned_name =
          generated_len > 0 && (size_t)generated_len < sizeof(generated_name)
              ? strdup(generated_name)
              : NULL;
      if (!bytes || !owned_name) {
        free(bytes);
        free(owned_name);
        fail(p, "out of memory creating a static C aggregate");
        ok = false;
        break;
      }
      c_global_t global = {.name = name,
                           .bytes = bytes,
                           .owned_name = owned_name,
                           .len = total_size,
                           .type = C_AGG,
                           .agg = decl_agg,
                           .is_array = is_array,
                           .owner_function = p->function ? p->function->name
                                                         : (ny_ctok_t){0}};
      size_t static_global_index = p->global_count;
      p->globals[p->global_count++] = global;
      if (has_initializer) {
        bool initialized = true;
        if (is_array) {
          initialized = expect(p, "{");
          size_t element = 0;
          bool first_element = true;
          while (initialized && !tok_is(p, p->pos, "}")) {
            if (!first_element && !expect(p, ",")) {
              initialized = false;
              break;
            }
            if (tok_is(p, p->pos, "}"))
              break;
            first_element = false;
            if (accept(p, "[")) {
              int64_t designated_index = -1;
              if (!parse_global_const_expr(p, &p->pos, &designated_index) ||
                  designated_index < 0 || (uint64_t)designated_index >= count) {
                fail(p, "invalid static aggregate array designator");
                initialized = false;
                break;
              }
              bool member_designator = tok_is(p, p->pos + 1, ".");
              if (!expect(p, "]") || (!member_designator && !expect(p, "="))) {
                fail(p, "invalid static aggregate array designator");
                initialized = false;
                break;
              }
              element = (size_t)designated_index;
            }
            if (element >= count) {
              fail(p,
                   "too many elements in static aggregate array initializer");
              initialized = false;
              break;
            }
            initialized = init_aggregate(p, &global, decl_agg, bytes,
                                         element * decl_agg->size, 0, -1);
            ++element;
          }
          if (initialized)
            (void)accept(p, ",");
          if (initialized)
            initialized = expect(p, "}");
        } else {
          initialized = init_aggregate(p, &global, decl_agg, bytes, 0, 0, -1);
        }
        if (!initialized) {
          free(global.bytes);
          free(global.owned_name);
          free(global.relocs);
          p->globals[static_global_index] = (c_global_t){0};
          if (!p->err[0])
            fail(p, "unsupported static C aggregate initializer");
          ok = false;
          break;
        }
        p->globals[static_global_index] = global;
      }
      if (local_section.len) {
        char section_name[64];
        if (!copy_quoted_token(p, local_section, section_name,
                               sizeof(section_name)) ||
            !(global.section = strdup(section_name))) {
          free(global.bytes);
          free(global.owned_name);
          free(global.relocs);
          p->globals[static_global_index] = (c_global_t){0};
          fail(p, "invalid or unavailable static local section name");
          ok = false;
          break;
        }
      }
      for (size_t i = 0; i < global.reloc_count; ++i)
        if (tok_same(global.relocs[i].target, name))
          global.relocs[i].target = (ny_ctok_t){.kind = NY_CTOK_IDENT,
                                                .start = owned_name,
                                                .len = (size_t)generated_len};
      global.name = (ny_ctok_t){.kind = NY_CTOK_IDENT,
                                .start = owned_name,
                                .len = (size_t)generated_len};
      p->globals[static_global_index] = global;
      if (p->local_count >= p->local_capacity || p->next_local == SIZE_MAX) {
        fail(p, "C frontend local-variable storage capacity exceeded");
        ok = false;
        break;
      }
      size_t slot = p->next_local++;
      p->locals[p->local_count++] =
          (c_local_t){.name = name,
                      .static_name = p->globals[static_global_index].name,
                      .cleanup = local_cleanup,
                      .type = C_AGG,
                      .agg = decl_agg,
                      .slot = slot,
                      .is_const = is_const,
                      .array_count = is_array ? count : 0};
      continue;
    }
    int initial_value = -1;
    size_t initializer_local = SIZE_MAX;
    bool has_initializer = accept(p, "=");
    if (has_initializer || (is_array && type != C_AGG)) {
      if (has_initializer && (type == C_AGG || c_type_is_pointer(type)) &&
          p->local_count < p->local_capacity && p->next_local != SIZE_MAX) {
        initializer_local = p->local_count++;
        size_t initializer_slot = p->next_local++;
        p->locals[initializer_local] = (c_local_t){.name = name,
                                                   .cleanup = local_cleanup,
                                                   .type = type,
                                                   .agg = decl_agg,
                                                   .slot = initializer_slot};
      }
      if (is_array && type != C_AGG) {
        if (!count && has_initializer && tok_is(p, p->pos, "{"))
          count = count_global_aggregate_array_initializer(p, p->pos);
        size_t element_size = p->abi == NYC_ABI_I386 ? 4 : 8;
        if (!count || count > SIZE_MAX / element_size ||
            (initializer_local == SIZE_MAX && p->next_local == SIZE_MAX) ||
            (initializer_local == SIZE_MAX &&
             p->local_count >= p->local_capacity)) {
          fail(p, "local aggregate-pointer array has an invalid size");
          ok = false;
          break;
        }
        size_t offset = 0;
        size_t bytes = count * element_size;
        p->has_auto_arrays = true;
        if (!agg_area_alloc(p, bytes, &offset)) {
          ok = false;
          break;
        }
        int base = lower_add_const(p, p->agg_base, offset);
        size_t slot = initializer_local == SIZE_MAX
                          ? p->next_local++
                          : p->locals[initializer_local].slot;
        c_local_t declared = {.name = name,
                              .cleanup = local_cleanup,
                              .type = type,
                              .agg = decl_agg,
                              .slot = slot,
                              .array_count = count,
                              .array_rank = 1,
                              .array_extent = {count},
                              .address_taken = true};
        if (base < 0 || (has_initializer && !lower_zero_fill(p, base, bytes)) ||
            !emit(p,
                  (nyr_inst_t){.op = NYIR_STORE_LOCAL,
                               .dst = -1,
                               .a = base,
                               .imm = (int64_t)slot,
                               .scalar_class = NYIR_SCALAR_I64_RAW},
                  NULL)) {
          ok = false;
          break;
        }
        if (initializer_local != SIZE_MAX)
          p->locals[initializer_local] = declared;
        else
          p->locals[p->local_count++] = declared;
        if (has_initializer) {
          if (!expect(p, "{")) {
            ok = false;
            break;
          }
          size_t element = 0;
          bool first_element = true;
          while (!tok_is(p, p->pos, "}")) {
            if (!first_element && !expect(p, ",")) {
              ok = false;
              break;
            }
            if (tok_is(p, p->pos, "}"))
              break;
            first_element = false;
            if (accept(p, "[")) {
              int64_t designated_index = -1;
              if (!parse_global_const_expr(p, &p->pos, &designated_index) ||
                  designated_index < 0 || (uint64_t)designated_index >= count ||
                  !expect(p, "]") || !expect(p, "=")) {
                fail(p, "invalid local aggregate-pointer array designator");
                ok = false;
                break;
              }
              element = (size_t)designated_index;
            }
            c_value_t value = parse_expr(p, 1);
            bool null_pointer = p->last_value_null_pointer_constant;
            if (value.value < 0 ||
                (!c_type_is_pointer(value.type) && !null_pointer) ||
                (c_type_is_pointer(value.type) && decl_agg && value.agg &&
                 value.agg != decl_agg)) {
              fail(p, "local aggregate-pointer array initializer requires a "
                      "compatible pointer or null pointer constant");
              ok = false;
              break;
            }
            int address = lower_add_const(p, base, element * element_size);
            if (address < 0 ||
                !lower_store_pointer_field(p, address, value.value, false)) {
              ok = false;
              break;
            }
            element = (size_t)(element + 1);
            if (element > count) {
              fail(p, "too many elements in local aggregate-pointer array");
              ok = false;
              break;
            }
          }
          if (ok)
            (void)accept(p, ",");
          if (ok)
            ok = expect(p, "}");
          if (!ok)
            break;
        }
        continue;
      }
      if (type == C_AGG && tok_is(p, p->pos, "{")) {
        if (!decl_agg || !decl_agg->complete ||
            p->global_count >= p->global_capacity) {
          fail(p, "local aggregate initializer requires a complete type");
          ok = false;
          break;
        }
        if (is_array && !count)
          count = count_global_aggregate_array_initializer(p, p->pos);
        if (!count || decl_agg->size > SIZE_MAX / count) {
          fail(p, "local aggregate initializer has an invalid array size");
          ok = false;
          break;
        }
        size_t total_size = decl_agg->size * count;
        bool zero_initializer = zero_aggregate_initializer(p);
        if (!literal_aggregate_initializer(p) || zero_initializer) {
          size_t offset = 0;
          if (initializer_local == SIZE_MAX || p->next_local == SIZE_MAX ||
              !agg_area_alloc(p, total_size, &offset)) {
            if (!p->err[0])
              fail(p, "C frontend local-variable storage capacity exceeded");
            ok = false;
            break;
          }
          int base = lower_add_const(p, p->agg_base, offset);
          size_t slot = p->next_local++;
          p->locals[initializer_local] =
              (c_local_t){.name = name,
                          .cleanup = local_cleanup,
                          .type = C_AGG,
                          .agg = decl_agg,
                          .slot = slot,
                          .is_const = is_const,
                          .array_count = is_array ? count : 0};
          if (base < 0 ||
              !emit(p,
                    (nyr_inst_t){.op = NYIR_STORE_LOCAL,
                                 .dst = -1,
                                 .a = base,
                                 .imm = (int64_t)slot,
                                 .scalar_class = NYIR_SCALAR_I64_RAW},
                    NULL) ||
              !lower_zero_fill(p, base, total_size)) {
            ok = false;
            break;
          }
          if (zero_initializer) {
            p->pos += 3;
            continue;
          }
          if (is_array) {
            ok = expect(p, "{");
            size_t element = 0;
            while (ok && element < count && !tok_is(p, p->pos, "}")) {
              if (element && !expect(p, ",")) {
                ok = false;
                break;
              }
              if (tok_is(p, p->pos, "}"))
                break;
              size_t range_end = element + 1;
              if (accept(p, "[")) {
                if (!parse_local_aggregate_array_designator(p, count, &element,
                                                            &range_end)) {
                  ok = false;
                  break;
                }
              }
              if (element >= count) {
                fail(p, "too many values in local aggregate array initializer");
                ok = false;
                break;
              }
              if (accept(p, "*")) {
                c_value_t source = parse_expr(p, 14);
                if (source.value < 0 || source.type != C_AGG_PTR ||
                    (source.agg && source.agg != decl_agg)) {
                  fail(p, "aggregate array copy initializer requires a "
                          "compatible aggregate pointer");
                  ok = false;
                  break;
                }
                int destination =
                    lower_add_const(p, base, element * decl_agg->size);
                if (destination < 0 ||
                    !lower_copy_struct(p, destination, source.value,
                                       decl_agg->size)) {
                  ok = false;
                  break;
                }
                if (range_end > element + 1 &&
                    !copy_local_aggregate_initializer_range(
                        p, base, decl_agg->size, element, range_end)) {
                  ok = false;
                  break;
                }
              } else {
                ok = init_aggregate(p, NULL, decl_agg, NULL,
                                    element * decl_agg->size, 0, base);
                if (ok && range_end > element + 1)
                  ok = copy_local_aggregate_initializer_range(
                      p, base, decl_agg->size, element, range_end);
              }
              element = range_end;
            }
            if (ok)
              (void)accept(p, ",");
            if (ok)
              ok = expect(p, "}");
          } else {
            ok = init_aggregate(p, NULL, decl_agg, NULL, 0, 0, base);
          }
          if (!ok)
            break;
          continue;
        }
        unsigned char *initializer_bytes = calloc(total_size, 1);
        char generated_name[48];
        int generated_len = snprintf(generated_name, sizeof(generated_name),
                                     "__nyc_local_init_%zu", p->global_count);
        char *owned_name =
            generated_len > 0 && (size_t)generated_len < sizeof(generated_name)
                ? strdup(generated_name)
                : NULL;
        if (!initializer_bytes || !owned_name) {
          free(initializer_bytes);
          free(owned_name);
          fail(p, "out of memory creating a local aggregate initializer");
          ok = false;
          break;
        }
        c_global_t initializer = {.name = {.kind = NY_CTOK_IDENT,
                                           .start = owned_name,
                                           .len = (size_t)generated_len},
                                  .bytes = initializer_bytes,
                                  .owned_name = owned_name,
                                  .len = total_size,
                                  .type = C_AGG,
                                  .agg = decl_agg,
                                  .is_array = is_array,
                                  .is_local = true};
        bool initialized = true;
        if (is_array) {
          initialized = expect(p, "{");
          size_t element = 0;
          while (initialized && element < count && !tok_is(p, p->pos, "}")) {
            if (element && !expect(p, ",")) {
              initialized = false;
              break;
            }
            if (tok_is(p, p->pos, "}"))
              break;
            size_t range_end = element + 1;
            if (accept(p, "[")) {
              if (!parse_local_aggregate_array_designator(p, count, &element,
                                                          &range_end)) {
                initialized = false;
                break;
              }
            }
            if (element >= count) {
              fail(p, "too many values in local aggregate array initializer");
              initialized = false;
              break;
            }
            size_t source_reloc_start = initializer.reloc_count;
            initialized =
                init_aggregate(p, &initializer, decl_agg, initializer_bytes,
                               element * decl_agg->size, 0, -1);
            if (initialized && range_end > element + 1)
              initialized = copy_local_aggregate_data_range(
                  p, &initializer, decl_agg->size, element, range_end,
                  source_reloc_start);
            element = range_end;
          }
          if (initialized)
            (void)accept(p, ",");
          if (initialized)
            initialized = expect(p, "}");
        } else {
          initialized = init_aggregate(p, &initializer, decl_agg,
                                       initializer_bytes, 0, 0, -1);
        }
        if (!initialized) {
          free(initializer_bytes);
          free(owned_name);
          free(initializer.relocs);
          if (!p->err[0]) {
            char message[128];
            snprintf(message, sizeof(message),
                     "unsupported local aggregate initializer near `%.48s`",
                     p->pos < p->token_count && p->tokens[p->pos].start
                         ? p->tokens[p->pos].start
                         : "<end>");
            fail(p, message);
          }
          ok = false;
          break;
        }
        initializer.owner_function =
            p->function ? p->function->name : (ny_ctok_t){0};
        p->globals[p->global_count++] = initializer;
        static const char data_prefix[] = "__nyc_data_";
        size_t symbol_len = sizeof(data_prefix) - 1 + (size_t)generated_len;
        char *symbol = malloc(symbol_len + 1);
        if (!symbol) {
          fail(p, "out of memory lowering a local aggregate initializer");
          ok = false;
          break;
        }
        memcpy(symbol, data_prefix, sizeof(data_prefix) - 1);
        memcpy(symbol + sizeof(data_prefix) - 1, owned_name,
               (size_t)generated_len);
        symbol[symbol_len] = '\0';
        initial_value = emit_value(
            p,
            (nyr_inst_t){.op = NYIR_ADDR_SYMBOL, .dst = -1, .symbol = symbol});
        free(symbol);
        if (initial_value < 0) {
          ok = false;
          break;
        }
      } else {
        c_value_t initial = parse_expr(p, 1);
        if (initial.value < 0) {
          ok = false;
          break;
        }
        if (type == C_AGG) {
          if (initial.type != C_AGG || initial.agg != decl_agg) {
            const char *expected_name = decl_agg && decl_agg->name.len
                                            ? decl_agg->name.start
                                            : "<anonymous>";
            const char *actual_name = initial.agg && initial.agg->name.len
                                          ? initial.agg->name.start
                                          : "<anonymous>";
            int expected_len =
                decl_agg && decl_agg->name.len
                    ? (int)(decl_agg->name.len > 60 ? 60 : decl_agg->name.len)
                    : 11;
            int actual_len =
                initial.agg && initial.agg->name.len
                    ? (int)(initial.agg->name.len > 60 ? 60
                                                       : initial.agg->name.len)
                    : 11;
            int variable_len = (int)(name.len > 48 ? 48 : name.len);
            char message[192];
            snprintf(message, sizeof(message),
                     "aggregate initializer for %.*s mismatched (expected "
                     "%.*s %zuB/%zu fields, got %.*s %zuB/%zu fields)",
                     variable_len, name.start, expected_len, expected_name,
                     decl_agg ? decl_agg->size : 0,
                     decl_agg ? decl_agg->field_count : 0, actual_len,
                     actual_name, initial.agg ? initial.agg->size : 0,
                     initial.agg ? initial.agg->field_count : 0);
            fail(p, message);
            ok = false;
            break;
          }
        } else if (!p->last_value_null_pointer_constant &&
                   !(initial.type == C_AGG_PTR && initial.agg == decl_agg) &&
                   !c_pointer_compatible(initial.type, type)) {
          char message[192];
          snprintf(
              message, sizeof(message),
              "aggregate pointer initializer type mismatch: %.*s <- %.*s",
              decl_agg
                  ? (int)(decl_agg->name.len > 60 ? 60 : decl_agg->name.len)
                  : 0,
              decl_agg ? decl_agg->name.start : "",
              initial.agg
                  ? (int)(initial.agg->name.len > 60 ? 60
                                                     : initial.agg->name.len)
                  : 0,
              initial.agg ? initial.agg->name.start : "");
          fail(p, message);
          ok = false;
          break;
        }
        initial_value = initial.value;
      }
    } else if (is_const && type == C_AGG_PTR) {
      fail(p, "const aggregate pointer declaration requires an initializer");
      ok = false;
      break;
    }
    size_t bytes = type == C_AGG ? decl_agg->size * count : 8;
    size_t offset = 0;
    if (!agg_area_alloc(p, bytes, &offset)) {
      ok = false;
      break;
    }
    int base = lower_add_const(p, p->agg_base, offset);
    if (base < 0) {
      ok = false;
      break;
    }
    if (type == C_AGG && !lower_zero_fill(p, base, bytes)) {
      ok = false;
      break;
    }
    if (initial_value >= 0 && type == C_AGG) {
      for (size_t element = 0; element < count; ++element) {
        int dst =
            element ? lower_add_const(p, base, element * decl_agg->size) : base;
        int src = element ? lower_add_const(p, initial_value,
                                            element * decl_agg->size)
                          : initial_value;
        if (dst < 0 || src < 0 ||
            !lower_copy_struct(p, dst, src, decl_agg->size)) {
          ok = false;
          break;
        }
      }
      if (!ok)
        break;
    }
    int stored_value = initial_value;
    if (type != C_AGG && stored_value < 0) {
      stored_value = emit_value(
          p, (nyr_inst_t){.op = NYIR_CONST_I64, .dst = -1, .imm = 0});
      if (stored_value < 0) {
        ok = false;
        break;
      }
    }
    if ((initializer_local == SIZE_MAX &&
         (p->local_count >= p->local_capacity || p->next_local == SIZE_MAX))) {
      fail(p, "C frontend local-variable storage capacity exceeded");
      ok = false;
      break;
    }
    size_t slot = initializer_local == SIZE_MAX
                      ? p->next_local++
                      : p->locals[initializer_local].slot;
    if (!emit(p,
              (nyr_inst_t){.op = NYIR_STORE_LOCAL,
                           .dst = -1,
                           .a = type == C_AGG ? base : stored_value,
                           .imm = (int64_t)slot,
                           .scalar_class = NYIR_SCALAR_I64_RAW},
              NULL)) {
      ok = false;
      break;
    }
    c_local_t declared = {.name = name,
                          .cleanup = local_cleanup,
                          .type = type,
                          .pointer_base_type = declarator_base_type,
                          .pointer_depth = type == C_AGG ? 0 : declared_stars,
                          .agg = decl_agg,
                          .slot = slot,
                          .is_const = is_const,
                          .array_count = is_array ? count : 0};
    if (initializer_local != SIZE_MAX)
      p->locals[initializer_local] = declared;
    else
      p->locals[p->local_count++] = declared;
  } while (accept(p, ","));
  if (!ok)
    return false;
  if (semicolon && !expect(p, ";"))
    return false;
  return true;
}

static bool lower_pointer_store(c_parser_t *p, c_value_t address,
                                bool read_only, bool wrapped);

static bool lower_scalar_assignment(c_parser_t *p, bool semicolon) {
  size_t statement_start = p->pos;
  ny_ctok_t name;
  if (!parse_name(p, &name))
    return false;
  c_local_t *local = find_local(p, name);
  c_global_t *global = local ? NULL : find_global(p, name);
  if (!local &&
      !(global && (global->type == C_AGG || global->type == C_AGG_PTR))) {
    fail(p, "assignment target must be a declared scalar local or "
            "parameter");
    return false;
  }
  if (local && local->array_rank && local->type != C_AGG &&
      local->type != C_AGG_PTR && accept(p, "[")) {
    int base = lower_local_load(p, local);
    int offset = -1;
    unsigned indexed = 0;
    do {
      c_value_t index = parse_expr(p, 1);
      if (index.value < 0 || !c_type_is_integer(index.type) ||
          !expect(p, "]")) {
        fail(p, "local array assignment index must be an integer");
        return false;
      }
      size_t stride = c_type_size(p->abi, local->type);
      for (unsigned dimension = indexed + 1; dimension < local->array_rank;
           ++dimension) {
        if (local->array_extent[dimension] &&
            stride > SIZE_MAX / local->array_extent[dimension]) {
          fail(p, "local array assignment stride overflows");
          return false;
        }
        stride *= local->array_extent[dimension];
      }
      if (stride > INT64_MAX)
        return false;
      int scale = emit_value(p, (nyr_inst_t){.op = NYIR_CONST_I64,
                                             .dst = -1,
                                             .imm = (int64_t)stride});
      int part = scale < 0 ? -1
                           : emit_value(p, (nyr_inst_t){.op = NYIR_MUL_I64,
                                                        .dst = -1,
                                                        .a = index.value,
                                                        .b = scale});
      offset = part < 0     ? -1
               : offset < 0 ? part
                            : emit_value(p, (nyr_inst_t){.op = NYIR_ADD_I64,
                                                         .dst = -1,
                                                         .a = offset,
                                                         .b = part});
      if (offset < 0)
        return false;
      ++indexed;
    } while (indexed < local->array_rank && accept(p, "["));
    if (indexed != local->array_rank)
      return false;
    int address = base < 0 ? -1
                           : emit_value(p, (nyr_inst_t){.op = NYIR_ADD_I64,
                                                        .dst = -1,
                                                        .a = base,
                                                        .b = offset});
    if (address < 0)
      return false;
    if (c_type_is_pointer(local->type) && tok_is(p, p->pos, "[")) {
      int pointer = lower_load_pointer_field(p, address, false);
      c_value_t indexed_value =
          pointer < 0 ? (c_value_t){-1, C_BAD, NULL}
                      : lower_pointer_index_rvalue(
                            p, (c_value_t){pointer, local->type, local->agg});
      const c_lvalue_t *lvalue =
          indexed_value.value >= 0 ? find_lvalue(p, indexed_value.value) : NULL;
      c_type_t pointer_type =
          lvalue ? c_add_pointer_levels(lvalue->type, 1) : C_BAD;
      if (!lvalue || pointer_type == C_BAD)
        return false;
      return lower_pointer_store(
          p, (c_value_t){lvalue->address, pointer_type, lvalue->agg},
          local->is_const || local->pointee_const, false);
    }
    c_type_t pointer_type = c_add_pointer_levels(local->type, 1);
    if (pointer_type == C_BAD)
      return false;
    return lower_pointer_store(p,
                               (c_value_t){address, pointer_type, local->agg},
                               local->is_const || local->pointee_const, false);
  }
  /*
   * Aggregate targets: `s = other;` copies through NYIR_COPY_STRUCT, and a
   * member path (`s.inner.f = v` / `s.inner = other`) stores through the
   * field address.
   */
  c_type_t target_type = local ? local->type : global->type;
  c_agg_type_t *target_agg = local ? local->agg : global->agg;
  bool target_const = local ? local->is_const : global->is_const;
  size_t target_array_count = local ? local->array_count
                              : global && global->is_array && global->agg
                                  ? global->agg->size && global->len
                                        ? global->len / global->agg->size
                                        : 1
                                  : 0;
  if (target_type == C_AGG && target_array_count && tok_is(p, p->pos, "->")) {
    target_type = C_AGG_PTR;
    target_array_count = 0;
  }
  int aggregate_pointer_override = -1;
  if (local &&
      ((target_type == C_OPAQUE_PTR && local->pointer_depth > 1) ||
       target_type == C_CHAR_PTR_PTR || target_type == C_I8_PTR_PTR ||
       target_type == C_U16_PTR_PTR || target_type == C_FUNC_PTR_PTR ||
       target_type == C_OPAQUE_PTR_PTR || target_type == C_OPAQUE_PTR_PTR_PTR ||
       target_type == C_AGG_PTR_PTR) &&
      accept(p, "[")) {
    int base = lower_local_load(p, local);
    c_type_t pointer_type = target_type;
    unsigned remaining = local->pointer_depth;
    c_type_t pointer_base_type = local->pointer_base_type;
    if (!remaining && target_type == C_AGG_PTR_PTR && local->agg) {
      remaining = 2;
      pointer_base_type = C_AGG;
    }
    for (;;) {
      c_value_t index = parse_expr(p, 1);
      if (index.value < 0 || !c_type_is_integer(index.type) ||
          !expect(p, "]")) {
        fail(p, "pointer-to-pointer assignment index must be an integer");
        return false;
      }
      int address = base < 0 ? -1
                             : lower_pointer_index_address(p, pointer_type,
                                                           base, index.value);
      if (address < 0)
        return false;
      c_type_t address_type = pointer_type;
      if (!accept(p, "[")) {
        if (address_type == C_AGG_PTR && local->agg && tok_is(p, p->pos, ".")) {
          aggregate_pointer_override = address;
          target_type = C_AGG;
          target_agg = local->agg;
          target_array_count = 0;
          break;
        }
        if (pointer_type == C_AGG_PTR_PTR && local->agg &&
            tok_is(p, p->pos, "->")) {
          aggregate_pointer_override =
              lower_load_pointer_field(p, address, false);
          if (aggregate_pointer_override < 0)
            return false;
          target_type = C_AGG_PTR;
          target_agg = local->agg;
          target_array_count = 0;
          break;
        }
        return lower_pointer_store(
            p, (c_value_t){address, address_type, local->agg},
            local->is_const || local->pointee_const, false);
      }
      if (remaining <= 1) {
        fail(p, "pointer assignment index exceeds declared pointer depth");
        return false;
      }
      base = lower_load_pointer_field(p, address, false);
      --remaining;
      pointer_type = c_add_pointer_levels(pointer_base_type, remaining);
      if (base < 0 || pointer_type == C_BAD || pointer_type == C_VOID_PTR)
        return false;
    }
  }
  if (target_type == C_AGG ||
      (target_type == C_AGG_PTR &&
       (tok_is(p, p->pos, "->") || tok_is(p, p->pos, "[")))) {
    int base;
    if (local) {
      base = aggregate_pointer_override >= 0 ? aggregate_pointer_override
                                             : lower_local_load(p, local);
    } else {
      if (global_register_use_fails(p, global))
        return false;
      static const char data_prefix[] = "__nyc_data_";
      static const char extern_prefix[] = "__nyc_extern_";
      const char *prefix =
          global->is_external || global->is_weak ? extern_prefix : data_prefix;
      size_t prefix_len = strlen(prefix);
      size_t symbol_len = prefix_len + global->name.len;
      char *symbol = malloc(symbol_len + 1);
      if (!symbol) {
        fail(p, "out of memory lowering a file-scope aggregate assignment");
        return false;
      }
      memcpy(symbol, prefix, prefix_len);
      memcpy(symbol + prefix_len, global->name.start, global->name.len);
      symbol[symbol_len] = '\0';
      base = emit_value(
          p, (nyr_inst_t){.op = NYIR_ADDR_SYMBOL, .dst = -1, .symbol = symbol});
      free(symbol);
      if (base >= 0 && target_type == C_AGG_PTR && !global->is_array)
        base = lower_load_pointer_field(p, base, false);
    }
    if (base < 0)
      return false;
    c_value_t target = {base, target_type, target_agg};
    bool descending = true;
    bool target_pointer_index = target_type == C_AGG_PTR && accept(p, "[");
    if (target_array_count > 0 || target_pointer_index ||
        (target_type == C_AGG_PTR ? accept(p, "->") : accept(p, "."))) {
      /*
       * Member (or array-element) path.
       */
      if (target_array_count > 0 || target_pointer_index) {
        if (target_array_count > 0 && !target_pointer_index &&
            !expect(p, "[")) {
          fail(p, "array assignment requires an element index");
          return false;
        }
        c_value_t index = parse_expr(p, 1);
        if (index.value < 0 || !c_type_is_integer(index.type) ||
            !expect(p, "]")) {
          fail(p, "array index must be a signed integer");
          return false;
        }
        bool pointer_array =
            target_type == C_AGG_PTR && ((local && local->array_rank) ||
                                         (global && global->is_pointer_array));
        int scale = emit_value(
            p, (nyr_inst_t){.op = NYIR_CONST_I64,
                            .dst = -1,
                            .imm = pointer_array ? (int64_t)c_type_size(
                                                       p->abi, C_VOID_PTR)
                                                 : (int64_t)target_agg->size});
        int offset = scale < 0 ? -1
                               : emit_value(p, (nyr_inst_t){.op = NYIR_MUL_I64,
                                                            .dst = -1,
                                                            .a = index.value,
                                                            .b = scale});
        int address = offset < 0
                          ? -1
                          : emit_value(p, (nyr_inst_t){.op = NYIR_ADD_I64,
                                                       .dst = -1,
                                                       .a = base,
                                                       .b = offset});
        if (address < 0)
          return false;
        if (pointer_array) {
          int pointer = lower_load_pointer_field(p, address, false);
          if (pointer < 0)
            return false;
          target = (c_value_t){pointer, C_AGG_PTR, target_agg};
          descending = accept(p, "->");
        } else {
          target = (c_value_t){address, C_AGG, target_agg};
          descending = accept(p, ".");
        }
      }
      while (descending) {
        ny_ctok_t field_name;
        if (!parse_name(p, &field_name))
          return false;
        const c_agg_field_t *field = find_field(target.agg, field_name);
        if (!field || !target.agg->complete) {
          if (p->err && p->err_len && !p->err[0])
            snprintf(p->err, p->err_len,
                     "%u:%u: unknown field `%.*s` in assignment target "
                     "for `%.*s` (aggregate has %zu fields, complete=%d)",
                     current(p)->line, current(p)->col, (int)field_name.len,
                     field_name.start,
                     target.agg ? (int)target.agg->name.len : 0,
                     target.agg ? target.agg->name.start : "",
                     target.agg ? target.agg->field_count : 0,
                     target.agg ? target.agg->complete : 0);
          return false;
        }
        if (target.value < 0)
          return false;
        int address = lower_add_const(p, target.value, field->offset);
        if (address < 0)
          return false;
        c_agg_field_kind_t field_kind = field->kind;
        if (field_kind == C_AGG_FIELD_AGG) {
          target = (c_value_t){address, C_AGG, field->agg};
          descending = accept(p, ".");
          continue;
        }
        if (field_kind == C_AGG_FIELD_PTR && field->agg && accept(p, "->")) {
          int pointer =
              lower_load_pointer_field(p, address, field->is_volatile);
          if (pointer < 0)
            return false;
          target = (c_value_t){pointer, C_AGG_PTR, field->agg};
          descending = true;
          continue;
        }
        if (field_kind == C_AGG_FIELD_PTR && field->agg && accept(p, "[")) {
          c_value_t index = parse_expr(p, 1);
          if (index.value < 0 || !c_type_is_integer(index.type) ||
              !expect(p, "]")) {
            fail(p, "aggregate pointer index must be an integer");
            return false;
          }
          int pointer =
              lower_load_pointer_field(p, address, field->is_volatile);
          int stride = emit_value(
              p,
              (nyr_inst_t){.op = NYIR_CONST_I64,
                           .dst = -1,
                           .imm = field->pointer_depth > 1
                                      ? (int64_t)c_type_size(p->abi, C_VOID_PTR)
                                      : (int64_t)field->agg->size});
          int offset = pointer < 0 || stride < 0
                           ? -1
                           : emit_value(p, (nyr_inst_t){.op = NYIR_MUL_I64,
                                                        .dst = -1,
                                                        .a = index.value,
                                                        .b = stride});
          int element = offset < 0
                            ? -1
                            : emit_value(p, (nyr_inst_t){.op = NYIR_ADD_I64,
                                                         .dst = -1,
                                                         .a = pointer,
                                                         .b = offset});
          if (element < 0)
            return false;
          if (field->elem_pointer) {
            address = element;
            if (field->agg && field->pointer_depth > 1 && accept(p, "[")) {
              c_value_t nested_index = parse_expr(p, 1);
              if (nested_index.value < 0 ||
                  !c_type_is_integer(nested_index.type) || !expect(p, "]")) {
                fail(p, "nested aggregate pointer index must be an integer");
                return false;
              }
              int pointer = emit_value(
                  p, (nyr_inst_t){.op = NYIR_LOAD_I64,
                                  .dst = -1,
                                  .a = address,
                                  .effects = NYIR_EFFECT_READ_MEMORY |
                                             NYIR_EFFECT_MAY_TRAP,
                                  .scalar_class = NYIR_SCALAR_I64_RAW});
              int stride =
                  emit_value(p, (nyr_inst_t){.op = NYIR_CONST_I64,
                                             .dst = -1,
                                             .imm = (int64_t)field->agg->size});
              int offset =
                  pointer < 0 || stride < 0
                      ? -1
                      : emit_value(p, (nyr_inst_t){.op = NYIR_MUL_I64,
                                                   .dst = -1,
                                                   .a = nested_index.value,
                                                   .b = stride});
              int aggregate =
                  offset < 0 ? -1
                             : emit_value(p, (nyr_inst_t){.op = NYIR_ADD_I64,
                                                          .dst = -1,
                                                          .a = pointer,
                                                          .b = offset});
              if (aggregate < 0)
                return false;
              target = (c_value_t){aggregate, C_AGG, field->agg};
              descending = accept(p, ".");
              continue;
            }
            if (field->agg && accept(p, "->")) {
              int pointer =
                  lower_load_pointer_field(p, address, field->is_volatile);
              if (pointer < 0)
                return false;
              target = (c_value_t){pointer, C_AGG_PTR, field->agg};
              descending = true;
              continue;
            }
          } else {
            target = (c_value_t){element, C_AGG, field->agg};
            descending = accept(p, ".");
            continue;
          }
        }
        if (field_kind == C_AGG_FIELD_PTR && !field->agg &&
            field->pointer_depth > 1) {
          p->pos = statement_start;
          c_value_t result = parse_expr(p, 0);
          return result.value >= 0 && (!semicolon || expect(p, ";"));
        }
        if (field_kind == C_AGG_FIELD_PTR && !field->agg && accept(p, "[")) {
          c_value_t index = parse_expr(p, 1);
          if (index.value < 0 || !c_type_is_integer(index.type) ||
              !expect(p, "]")) {
            fail(p, "pointer field index must be an integer");
            return false;
          }
          int pointer =
              lower_load_pointer_field(p, address, field->is_volatile);
          size_t stride = field->is_function_pointer || field->elem_pointer
                              ? c_type_size(p->abi, C_FUNC_PTR)
                              : field->elem_size;
          int offset = index.value;
          if (stride > 1) {
            int scale = emit_value(p, (nyr_inst_t){.op = NYIR_CONST_I64,
                                                   .dst = -1,
                                                   .imm = (int64_t)stride});
            offset = scale < 0 ? -1
                               : emit_value(p, (nyr_inst_t){.op = NYIR_MUL_I64,
                                                            .dst = -1,
                                                            .a = index.value,
                                                            .b = scale});
          }
          address = pointer < 0 || offset < 0
                        ? -1
                        : emit_value(p, (nyr_inst_t){.op = NYIR_ADD_I64,
                                                     .dst = -1,
                                                     .a = pointer,
                                                     .b = offset});
          if (address < 0)
            return false;
          if (!field->is_function_pointer && !field->elem_pointer)
            field_kind =
                field->elem_f64         ? C_AGG_FIELD_F64
                : field->elem_size == 1 ? C_AGG_FIELD_U8
                : field->elem_size == 2
                    ? (field->elem_unsigned ? C_AGG_FIELD_U16 : C_AGG_FIELD_I16)
                : field->elem_size == 4
                    ? (field->elem_unsigned ? C_AGG_FIELD_U32 : C_AGG_FIELD_I32)
                    : (field->elem_unsigned ? C_AGG_FIELD_U64
                                            : C_AGG_FIELD_I64);
        }
        if (field_kind == C_AGG_FIELD_ARRAY) {
          if (field->array_rank > 1) {
            p->pos = statement_start;
            c_value_t result = parse_expr(p, 0);
            return result.value >= 0 && (!semicolon || expect(p, ";"));
          }
          if (!accept(p, "[")) {
            if (field->agg && !field->elem_pointer && accept(p, "->")) {
              target = (c_value_t){address, C_AGG_PTR, field->agg};
              descending = true;
              continue;
            }
            fail(p, "array field assignment requires an index");
            return false;
          }
          c_value_t index = parse_expr(p, 1);
          if (index.value < 0 || !c_type_is_integer(index.type) ||
              !expect(p, "]")) {
            fail(p, "array index must be an integer");
            return false;
          }
          int offset = index.value;
          if (field->elem_size != 1) {
            int stride =
                emit_value(p, (nyr_inst_t){.op = NYIR_CONST_I64,
                                           .dst = -1,
                                           .imm = (int64_t)field->elem_size});
            if (stride < 0)
              return false;
            offset = emit_value(p, (nyr_inst_t){.op = NYIR_MUL_I64,
                                                .dst = -1,
                                                .a = index.value,
                                                .b = stride});
            if (offset < 0)
              return false;
          }
          address = emit_value(
              p, (nyr_inst_t){
                     .op = NYIR_ADD_I64, .dst = -1, .a = address, .b = offset});
          if (address < 0)
            return false;
          if (field->agg && field->elem_pointer && accept(p, "->")) {
            int pointer =
                lower_load_pointer_field(p, address, field->is_volatile);
            if (pointer < 0)
              return false;
            target = (c_value_t){pointer, C_AGG_PTR, field->agg};
            descending = true;
            continue;
          }
          if (field->agg && !field->elem_pointer) {
            target = (c_value_t){address, C_AGG, field->agg};
            descending = accept(p, ".");
            continue;
          }
          field_kind =
              field->elem_pointer     ? C_AGG_FIELD_PTR
              : field->elem_f64       ? C_AGG_FIELD_F64
              : field->elem_size == 1 ? C_AGG_FIELD_U8
              : field->elem_size == 2
                  ? (field->elem_unsigned ? C_AGG_FIELD_U16 : C_AGG_FIELD_I16)
              : field->elem_size == 4
                  ? (field->elem_unsigned ? C_AGG_FIELD_U32 : C_AGG_FIELD_I32)
                  : (field->elem_unsigned ? C_AGG_FIELD_U64 : C_AGG_FIELD_I64);
          if (field->elem_pointer && accept(p, "[")) {
            c_value_t pointer_index = parse_expr(p, 1);
            if (pointer_index.value < 0 ||
                !c_type_is_integer(pointer_index.type) || !expect(p, "]")) {
              fail(p, "pointer-array field index must be an integer");
              return false;
            }
            int pointer =
                lower_load_pointer_field(p, address, field->is_volatile);
            size_t pointer_size = c_type_size(p->abi, C_VOID_PTR);
            int stride =
                pointer_size
                    ? emit_value(p, (nyr_inst_t){.op = NYIR_CONST_I64,
                                                 .dst = -1,
                                                 .imm = (int64_t)pointer_size})
                    : -1;
            int offset =
                pointer < 0 || stride < 0
                    ? -1
                    : emit_value(p, (nyr_inst_t){.op = NYIR_MUL_I64,
                                                 .dst = -1,
                                                 .a = pointer_index.value,
                                                 .b = stride});
            address = offset < 0
                          ? -1
                          : emit_value(p, (nyr_inst_t){.op = NYIR_ADD_I64,
                                                       .dst = -1,
                                                       .a = pointer,
                                                       .b = offset});
            if (address < 0)
              return false;
          }
        }
        /*
         * Scalar leaf: store through the field address.
         */
        if (field->is_bitfield) {
          p->pos = statement_start;
          c_value_t result = parse_expr(p, 0);
          return result.value >= 0 && (!semicolon || expect(p, ";"));
        }
        bool is_f32 = field_kind == C_AGG_FIELD_F32;
        bool is_f64 = field_kind == C_AGG_FIELD_F64;
        bool is_float_field = is_f32 || is_f64;
        bool is_i32 = field_kind == C_AGG_FIELD_I32;
        bool is_ptr = field_kind == C_AGG_FIELD_PTR;
        bool is_u8 = field_kind == C_AGG_FIELD_U8;
        bool is_u32 = field_kind == C_AGG_FIELD_U32;
        bool is_u64 = field_kind == C_AGG_FIELD_U64;
        bool is_i16 = field_kind == C_AGG_FIELD_I16;
        bool is_u16 = field_kind == C_AGG_FIELD_U16;
        c_type_t field_pointer_type = field->agg              ? C_AGG_PTR
                                      : field->elem_size == 1 ? C_CHAR_PTR
                                      : field->elem_size == 4 ? C_I32_PTR
                                                              : C_I64_PTR;
        c_type_t field_type = is_f32   ? C_F32
                              : is_f64 ? C_F64
                              : is_i32 ? C_I32
                              : is_u32 ? C_U32
                              : is_u8  ? (field->is_signed_byte ? C_CHAR : C_U8)
                              : is_i16 ? C_I16
                              : is_u16 ? C_U16
                              : is_u64 ? C_U64
                                       : C_I64;
        bool is_integer_field = is_i32 || is_u32 || is_u8 || is_i16 || is_u16 ||
                                is_u64 || field_kind == C_AGG_FIELD_I64;
        if (is_integer_field && (accept(p, "++") || accept(p, "--"))) {
          bool decrement = ny_ctok_eq(p->tokens[p->pos - 1], "--");
          int previous =
              is_i32 ? lower_load_i32_field(p, address, field->is_volatile)
              : is_u32
                  ? emit_value(
                        p,
                        (nyr_inst_t){
                            .op = NYIR_LOAD_I64,
                            .dst = -1,
                            .a = address,
                            .effects =
                                NYIR_EFFECT_READ_MEMORY | NYIR_EFFECT_MAY_TRAP |
                                (field->is_volatile ? NYIR_EFFECT_VOLATILE : 0),
                            .flags = NYIR_INST_F_MEM_I32,
                            .scalar_class = NYIR_SCALAR_I64_RAW})
              : is_u8 ? lower_load_u8_field(p, address, field->is_volatile)
              : is_i16 || is_u16 ? lower_load_u16_field(p, address, field_type,
                                                        field->is_volatile)
              : is_u64
                  ? lower_load_field(p, address, false, field->is_volatile)
                  : lower_load_field(p, address, false, field->is_volatile);
          if (previous < 0)
            return false;
          if (is_u32) {
            previous = normalize_u32(p, previous);
            if (previous < 0)
              return false;
          }
          int one = emit_i64_constant(p, 1);
          int updated =
              one < 0 ? -1
                      : emit_value(p, (nyr_inst_t){
                                          .op = decrement ? NYIR_SUB_I64
                                                          : NYIR_ADD_I64,
                                          .dst = -1,
                                          .a = previous,
                                          .b = one,
                                          .scalar_class = NYIR_SCALAR_I64_RAW});
          if (updated < 0)
            return false;
          if (c_type_is_integer(field_type))
            updated = normalize_integer(p, field_type, updated);
          if (updated < 0)
            return false;
          if (semicolon && !expect(p, ";"))
            return false;
          return is_i32 || is_u32 ? lower_store_i32_field(p, address, updated,
                                                          field->is_volatile)
                 : is_u8          ? lower_store_u8_field(p, address, updated,
                                                         field->is_volatile)
                 : is_i16 || is_u16
                     ? lower_store_u16_field(p, address, updated, field_type,
                                             field->is_volatile)
                 : is_ptr ? lower_store_pointer_field(p, address, updated,
                                                      field->is_volatile)
                          : lower_store_field(p, address, updated, false,
                                              field->is_volatile);
        }
        if (is_ptr && (accept(p, "++") || accept(p, "--"))) {
          bool decrement = ny_ctok_eq(p->tokens[p->pos - 1], "--");
          int64_t scale =
              c_pointer_scale_for(p->abi, field_pointer_type, field->agg);
          int previous =
              lower_load_pointer_field(p, address, field->is_volatile);
          int step = scale > 0 ? emit_i64_constant(p, scale) : -1;
          int updated =
              previous < 0 || step < 0
                  ? -1
                  : emit_value(
                        p, (nyr_inst_t){.op = decrement ? NYIR_SUB_I64
                                                        : NYIR_ADD_I64,
                                        .dst = -1,
                                        .a = previous,
                                        .b = step,
                                        .scalar_class = NYIR_SCALAR_I64_RAW});
          if (updated < 0 || (semicolon && !expect(p, ";")))
            return false;
          return lower_store_pointer_field(p, address, updated,
                                           field->is_volatile);
        }
        if (!is_assignment_operator(p, p->pos)) {
          p->pos = statement_start;
          (void)parse_expr(p, 0);
          return p->pos > statement_start &&
                 (!p->err || !p->err_len || !p->err[0]) &&
                 (!semicolon || expect(p, ";"));
        }
        ny_ctok_t assignment_op = *current(p);
        bool simple = ny_ctok_eq(assignment_op, "=");
        bool pointer_offset =
            is_ptr && c_type_is_arithmetic_pointer(field_pointer_type) &&
            (ny_ctok_eq(assignment_op, "+=") ||
             ny_ctok_eq(assignment_op, "-="));
        bool float_compound =
            is_float_field && (ny_ctok_eq(assignment_op, "+=") ||
                               ny_ctok_eq(assignment_op, "-=") ||
                               ny_ctok_eq(assignment_op, "*=") ||
                               ny_ctok_eq(assignment_op, "/="));
        if (!simple && !is_integer_field && !float_compound &&
            !pointer_offset) {
          fail(p, "compound assignment is unsupported for this aggregate "
                  "field type");
          return false;
        }
        ++p->pos;
        c_value_t value = parse_expr(p, 1);
        bool compatible =
            pointer_offset ? c_type_is_integer(value.type)
            : is_float_field
                ? c_type_is_float(value.type) || c_type_is_integer(value.type)
            : is_ptr ? c_type_is_pointer(value.type) ||
                           (c_type_is_integer(value.type) &&
                            p->last_value_null_pointer_constant)
                     : (field->is_bool && c_type_is_pointer(value.type)) ||
                           (c_type_is_integer(field_type) &&
                            c_type_is_integer(value.type));
        if (value.value < 0 || !compatible) {
          if (value.value >= 0) {
            fail(p, "field assignment requires a compatible value");
          }
          return false;
        }
        if (field->is_bool && c_type_is_pointer(value.type)) {
          int zero = emit_i64_constant(p, 0);
          value.value =
              zero < 0 ? -1
                       : emit_i64_compare(p, value.value, zero, NYIR_CMP_NE);
          value.type = C_U8;
          if (value.value < 0)
            return false;
        }
        if (is_float_field) {
          if (c_type_is_integer(value.type)) {
            if (value.type == C_U64) {
              nyr_inst_t call = {
                  .op = NYIR_CALL,
                  .dst = -1,
                  .a = value.value,
                  .imm = 1,
                  .flags = NYIR_INST_F_EXTERN,
                  .effects = NYIR_EFFECT_CALL,
                  .scalar_class = c_float_scalar_class(field_type),
                  .symbol = is_f32 ? "__floatundisf" : "__floatundidf"};
              nyr_call_arg_class_set(call.arg_scalar_classes, 0,
                                     NYIR_SCALAR_I64_RAW);
              value.value = emit_value(p, call);
            } else {
              value.value = emit_value(
                  p, (nyr_inst_t){
                         .op = is_f32 ? NYIR_I64_TO_F32 : NYIR_I64_TO_F64,
                         .dst = -1,
                         .a = value.value,
                         .scalar_class = c_float_scalar_class(field_type)});
            }
          } else if (value.type != field_type) {
            value.value = emit_value(
                p,
                (nyr_inst_t){.op = is_f32 ? NYIR_F64_TO_F32 : NYIR_F32_TO_F64,
                             .dst = -1,
                             .a = value.value,
                             .scalar_class = c_float_scalar_class(field_type)});
          }
          value.type = field_type;
          if (value.value < 0)
            return false;
        }
        if (!simple) {
          int previous =
              pointer_offset
                  ? lower_load_pointer_field(p, address, field->is_volatile)
              : is_f32 ? lower_load_f32_field(p, address, field->is_volatile)
              : is_i32 ? lower_load_i32_field(p, address, field->is_volatile)
              : is_u32
                  ? emit_value(
                        p,
                        (nyr_inst_t){
                            .op = NYIR_LOAD_I64,
                            .dst = -1,
                            .a = address,
                            .effects =
                                NYIR_EFFECT_READ_MEMORY | NYIR_EFFECT_MAY_TRAP |
                                (field->is_volatile ? NYIR_EFFECT_VOLATILE : 0),
                            .flags = NYIR_INST_F_MEM_I32,
                            .scalar_class = NYIR_SCALAR_I64_RAW})
              : is_u8 ? lower_load_u8_field(p, address, field->is_volatile)
              : is_i16 || is_u16
                  ? lower_load_u16_field(p, address, field_type,
                                         field->is_volatile)
                  : lower_load_field(p, address, false, field->is_volatile);
          if (previous >= 0 && c_type_is_integer(field_type))
            previous = normalize_integer(p, field_type, previous);
          if (pointer_offset) {
            value.value = lower_pointer_offset_for(
                p, field_pointer_type, field->agg, previous, value.value,
                ny_ctok_eq(assignment_op, "-="));
            if (value.value < 0)
              return false;
          }
          nyr_op_t operation =
              pointer_offset
                  ? NYIR_NOP
                  : compound_assignment_ir(field_type, assignment_op);
          if (previous < 0 || (!pointer_offset && operation == NYIR_NOP))
            return false;
          if (!pointer_offset) {
            value.value = emit_value(
                p, (nyr_inst_t){.op = operation,
                                .dst = -1,
                                .a = previous,
                                .b = value.value,
                                .scalar_class =
                                    is_float_field
                                        ? c_float_scalar_class(field_type)
                                        : NYIR_SCALAR_I64_RAW});
            if (value.value < 0)
              return false;
          }
        }
        if (c_type_is_integer(field_type)) {
          value.value = normalize_integer(p, field_type, value.value);
          if (value.value < 0)
            return false;
        }
        if (semicolon && !expect(p, ";"))
          return false;
        return is_f32 ? lower_store_f32_field(p, address, value.value,
                                              field->is_volatile)
               : is_i32 || is_u32
                   ? lower_store_i32_field(p, address, value.value,
                                           field->is_volatile)
               : is_u8 ? lower_store_u8_field(p, address, value.value,
                                              field->is_volatile)
               : is_i16 || is_u16
                   ? lower_store_u16_field(p, address, value.value, field_type,
                                           field->is_volatile)
               : c_type_is_pointer(field_type)
                   ? lower_store_pointer_field(p, address, value.value,
                                               field->is_volatile)
                   : lower_store_field(p, address, value.value, is_f64,
                                       field->is_volatile);
      }
      if (target.agg && target.agg->is_int128 &&
          ny_ctok_eq(*current(p), "<<=")) {
        if (target_const) {
          fail(p, "cannot assign to a const 128-bit integer");
          return false;
        }
        ++p->pos;
        c_value_t count = parse_expr(p, 1);
        c_value_t shifted =
            count.value < 0
                ? (c_value_t){-1, C_BAD, NULL}
                : lower_int128_shift_left(
                      p, (c_value_t){target.value, C_AGG, target.agg}, count);
        if (shifted.value < 0 || (semicolon && !expect(p, ";")))
          return false;
        return lower_copy_struct(p, target.value, shifted.value,
                                 target.agg->size);
      }
      if (target.agg && target.agg->is_int128 &&
          ny_ctok_eq(*current(p), "+=")) {
        if (target_const) {
          fail(p, "cannot assign to a const 128-bit integer");
          return false;
        }
        ++p->pos;
        c_value_t addend = parse_expr(p, 1);
        c_value_t sum =
            addend.value < 0
                ? (c_value_t){-1, C_BAD, NULL}
                : lower_int128_add_u64(
                      p, (c_value_t){target.value, C_AGG, target.agg}, addend);
        if (sum.value < 0 || (semicolon && !expect(p, ";")))
          return false;
        return lower_copy_struct(p, target.value, sum.value, target.agg->size);
      }
      if (target.agg && target.agg->is_int128 &&
          ny_ctok_eq(*current(p), ">>=")) {
        if (target_const || target.agg->fields[0].kind != C_AGG_FIELD_U64) {
          fail(p, "compound right shift currently requires unsigned __int128");
          return false;
        }
        ++p->pos;
        c_value_t count = parse_expr(p, 1);
        c_value_t shifted =
            count.value < 0
                ? (c_value_t){-1, C_BAD, NULL}
                : lower_int128_shift_right(
                      p, (c_value_t){target.value, C_AGG, target.agg}, count);
        if (shifted.value < 0 || (semicolon && !expect(p, ";")))
          return false;
        return lower_copy_struct(p, target.value, shifted.value,
                                 target.agg->size);
      }
      if (target.agg && target.agg->is_int128 &&
          (ny_ctok_eq(*current(p), "|=") || ny_ctok_eq(*current(p), "&=") ||
           ny_ctok_eq(*current(p), "^="))) {
        if (target_const) {
          fail(p, "cannot assign to a const 128-bit integer");
          return false;
        }
        ny_ctok_t assignment = *current(p);
        ++p->pos;
        c_value_t value = parse_expr(p, 1);
        nyr_op_t operation = ny_ctok_eq(assignment, "|=")   ? NYIR_OR_I64
                             : ny_ctok_eq(assignment, "&=") ? NYIR_AND_I64
                                                            : NYIR_XOR_I64;
        if (value.value < 0 ||
            !lower_int128_compound_bitwise(p, target.value, target.agg, value,
                                           operation) ||
            (semicolon && !expect(p, ";")))
          return false;
        return true;
      }
      if (!is_assignment_operator(p, p->pos) || !ny_ctok_eq(*current(p), "=")) {
        fail(p, "only simple assignment is supported for struct targets");
        return false;
      }
      ++p->pos;
      c_value_t value = parse_expr(p, 1);
      if (target.agg && target.agg->is_int128 &&
          c_type_is_integer(value.type)) {
        if (value.value < 0 ||
            !lower_int128_store_integer(p, target.value, value) ||
            (semicolon && !expect(p, ";")))
          return false;
        return true;
      }
      if (local && local->array_count && value.value >= 0 &&
          value.type == C_AGG_PTR && value.agg == target.agg) {
        if (semicolon && !expect(p, ";"))
          return false;
        return lower_store_pointer_field(p, target.value, value.value, false);
      }
      if (value.value >= 0 &&
          (value.type == C_AGG && value.agg == target.agg)) {
        if (semicolon && !expect(p, ";"))
          return false;
        return lower_copy_struct(p, target.value, value.value,
                                 target.agg->size);
      }
      if (value.value >= 0 && target.agg && getenv("NYC_KERNEL_COMPAT")) {
        if (value.type == C_AGG && value.agg &&
            value.agg->size == target.agg->size) {
          if (semicolon && !expect(p, ";"))
            return false;
          return lower_copy_struct(p, target.value, value.value,
                                   target.agg->size);
        }
        if (c_type_is_pointer(value.type) && value.type != C_AGG_PTR) {
          if (semicolon && !expect(p, ";"))
            return false;
          return lower_copy_struct(p, target.value, value.value,
                                   target.agg->size);
        }
        if (value.type == C_AGG_PTR) {
          if (semicolon && !expect(p, ";"))
            return false;
          return lower_copy_struct(p, target.value, value.value,
                                   target.agg->size);
        }
      }
      if (value.value < 0 || value.type != C_AGG || value.agg != target.agg) {
        if (value.value >= 0) {
          char message[144];
          snprintf(message, sizeof(message),
                   "aggregate assignment mismatch (target type %d/tag %p, "
                   "value type %d/tag %p)",
                   (int)target.type, (const void *)target.agg, (int)value.type,
                   (const void *)value.agg);
          fail(p, message);
        }
        return false;
      }
      if (semicolon && !expect(p, ";"))
        return false;
      return lower_copy_struct(p, target.value, value.value, target.agg->size);
    }
    if (target_const) {
      fail(p, "cannot assign to a const C object");
      return false;
    }
    if (local && target_agg && target_agg->is_int128 &&
        ny_ctok_eq(*current(p), "=")) {
      c_parser_t zero_probe = *p;
      size_t zero_pos = p->pos + 1;
      int64_t zero_value = 1;
      if (parse_global_const_expr(&zero_probe, &zero_pos, &zero_value) &&
          zero_value == 0 &&
          (tok_is(&zero_probe, zero_pos, ";") ||
           tok_is(&zero_probe, zero_pos, ",") ||
           tok_is(&zero_probe, zero_pos, ")"))) {
        ++p->pos;
        c_value_t zero_expr = parse_expr(p, 1);
        int zero = emit_i64_constant(p, 0);
        if (zero_expr.value < 0 || base < 0 || zero < 0 ||
            lower_int128_store(p, base, 0, zero) < 0 ||
            lower_int128_store(p, base, 8, zero) < 0 ||
            (semicolon && !expect(p, ";")))
          return false;
        return true;
      }
    }
    if (target_agg && target_agg->is_int128 && ny_ctok_eq(*current(p), "<<=")) {
      ++p->pos;
      c_value_t count = parse_expr(p, 1);
      c_value_t shifted =
          count.value < 0 ? (c_value_t){-1, C_BAD, NULL}
                          : lower_int128_shift_left(
                                p, (c_value_t){base, C_AGG, target_agg}, count);
      if (shifted.value < 0 || (semicolon && !expect(p, ";")))
        return false;
      return lower_copy_struct(p, base, shifted.value, target_agg->size);
    }
    if (target_agg && target_agg->is_int128 && ny_ctok_eq(*current(p), "+=")) {
      ++p->pos;
      c_value_t addend = parse_expr(p, 1);
      c_value_t sum =
          addend.value < 0
              ? (c_value_t){-1, C_BAD, NULL}
              : lower_int128_add_u64(p, (c_value_t){base, C_AGG, target_agg},
                                     addend);
      if (sum.value < 0 || (semicolon && !expect(p, ";")))
        return false;
      return lower_copy_struct(p, base, sum.value, target_agg->size);
    }
    if (target_agg && target_agg->is_int128 && ny_ctok_eq(*current(p), ">>=")) {
      if (target_agg->fields[0].kind != C_AGG_FIELD_U64) {
        fail(p, "compound right shift currently requires unsigned __int128");
        return false;
      }
      ++p->pos;
      c_value_t count = parse_expr(p, 1);
      c_value_t shifted =
          count.value < 0 ? (c_value_t){-1, C_BAD, NULL}
                          : lower_int128_shift_right(
                                p, (c_value_t){base, C_AGG, target_agg}, count);
      if (shifted.value < 0 || (semicolon && !expect(p, ";")))
        return false;
      return lower_copy_struct(p, base, shifted.value, target_agg->size);
    }
    if (!is_assignment_operator(p, p->pos) || !ny_ctok_eq(*current(p), "=")) {
      fail(p, "only simple assignment is supported for struct targets");
      return false;
    }
    ++p->pos;
    c_value_t value = parse_expr(p, 1);
    if (local && local->array_count && local->pointer_depth &&
        value.value >= 0 && value.type == C_AGG_PTR &&
        value.agg == target_agg) {
      if (semicolon && !expect(p, ";"))
        return false;
      return lower_store_pointer_field(p, base, value.value, false);
    }
    if (value.value < 0 || value.type != C_AGG || value.agg != target_agg) {
      if (value.value >= 0) {
        char message[256];
        snprintf(
            message, sizeof(message),
            "aggregate assignment mismatch (target type %d/tag %p, "
            "value type %d/tag %p, local type %d/depth %u/count %zu/rank %u)",
            (int)target_type, (const void *)target_agg, (int)value.type,
            (const void *)value.agg, local ? (int)local->type : -1,
            local ? local->pointer_depth : 0, local ? local->array_count : 0,
            local ? local->array_rank : 0);
        fail(p, message);
      }
      return false;
    }
    if (semicolon && !expect(p, ";"))
      return false;
    return lower_copy_struct(p, base, value.value, target_agg->size);
  }
  if (!local) {
    fail(p, "scalar assignment target must be a local or parameter");
    return false;
  }
  bool pointer = c_type_is_pointer(local->type);
  if (local->is_const && !pointer) {
    fail(p, "cannot assign to a const C object");
    return false;
  }
  if (!is_assignment_operator(p, p->pos)) {
    (void)expect(p, "=");
    return false;
  }
  ny_ctok_t op = *current(p);
  bool simple = ny_ctok_eq(op, "=");
  ++p->pos;
  bool pointer_offset = c_type_is_arithmetic_pointer(local->type) &&
                        (ny_ctok_eq(op, "+=") || ny_ctok_eq(op, "-="));
  if (!simple && !c_type_is_integer(local->type) &&
      !c_type_is_float(local->type) && !pointer_offset) {
    fail(p, "compound assignment operator is unsupported for this type");
    return false;
  }
  c_value_t value = parse_expr(p, 1);
  if (value.value >= 0 && c_type_is_float(local->type) &&
      (c_type_is_integer(value.type) || c_type_is_float(value.type)))
    value = convert_numeric_float(p, value, local->type);
  if (value.value >= 0 && local->is_bool && c_type_is_pointer(value.type)) {
    value.value = lower_boolean_value(p, value);
    if (value.value < 0)
      return false;
    value.type = local->type;
  }
  c_type_t expected_type = pointer_offset ? C_I64 : local->type;
  if (value.value < 0 ||
      !(value.type == expected_type ||
        c_pointer_compatible(value.type, expected_type) ||
        (c_type_is_integer(value.type) && c_type_is_integer(expected_type)) ||
        (pointer && p->last_value_null_pointer_constant) ||
        (getenv("NYC_KERNEL_COMPAT") &&
         ((c_type_is_pointer(expected_type) && c_type_is_integer(value.type)) ||
          (c_type_is_integer(expected_type) && c_type_is_pointer(value.type)) ||
          (c_type_is_pointer(expected_type) &&
           c_type_is_pointer(value.type)))))) {
    if (value.value >= 0) {
      char message[128];
      snprintf(message, sizeof(message),
               "assignment type mismatch: target %d, value %d, expected %d",
               local->type, value.type, expected_type);
      fail(p, message);
    }
    return false;
  }
  if (!simple) {
    int old_value = lower_local_load(p, local);
    nyr_op_t ir_op = compound_assignment_ir(local->type, op);
    if (pointer_offset) {
      if (old_value < 0)
        return false;
      value.value =
          lower_pointer_offset_for(p, local->type, local->agg, old_value,
                                   value.value, ny_ctok_eq(op, "-="));
      value.type = local->type;
    } else if ((local->type == C_U32 || local->type == C_U64) &&
               (ny_ctok_eq(op, "/=") || ny_ctok_eq(op, "%="))) {
      bool wide = local->type == C_U64;
      if (!wide) {
        old_value = normalize_u32(p, old_value);
        value.value = normalize_u32(p, value.value);
      }
      if (old_value < 0 || value.value < 0) {
        fail(p, "failed to lower unsigned compound division");
        return false;
      }
      value.value =
          emit_value(p, (nyr_inst_t){.op = NYIR_INLINE_ASM,
                                     .dst = -1,
                                     .a = old_value,
                                     .b = value.value,
                                     .imm = ny_ctok_eq(op, "/=") ? -21 : -22,
                                     .effects = NYIR_EFFECT_VOLATILE,
                                     .symbol = "unsigned-div"});
      if (!wide && value.value >= 0)
        value.value = normalize_u32(p, value.value);
      value.type = local->type;
    } else if (local->type == C_U64 && ny_ctok_eq(op, ">>=")) {
      if (old_value < 0 || value.value < 0) {
        fail(p, "failed to lower unsigned right shift");
        return false;
      }
      value.value =
          emit_value(p, (nyr_inst_t){.op = NYIR_INLINE_ASM,
                                     .dst = -1,
                                     .a = old_value,
                                     .b = value.value,
                                     .imm = -23,
                                     .effects = NYIR_EFFECT_NONE,
                                     .symbol = "unsigned-shift-right"});
      value.type = C_U64;
    } else if (old_value < 0 || ir_op == NYIR_NOP) {
      fail(p, "compound assignment operator is unsupported for this type");
      return false;
    } else {
      value.value =
          emit_value(p, (nyr_inst_t){.op = ir_op,
                                     .dst = -1,
                                     .a = old_value,
                                     .b = value.value,
                                     .scalar_class =
                                         c_type_is_float(local->type)
                                             ? c_float_scalar_class(local->type)
                                             : NYIR_SCALAR_I64_RAW});
    }
    if (value.value < 0)
      return false;
  }
  if (c_type_is_integer(local->type)) {
    value.value = normalize_integer(p, local->type, value.value);
    if (value.value < 0)
      return false;
  }
  if (semicolon && !expect(p, ";"))
    return false;
  return lower_local_store(p, local, value.value);
}

static bool lower_pointer_store(c_parser_t *p, c_value_t address,
                                bool read_only, bool wrapped) {
  /*
   * `(*ptr) op= value`: the address parse stops inside the grouping; skip
   * to the closing paren so the assignment operator follows naturally.
   */
  if (wrapped && !accept(p, ")"))
    return false;
  if (!wrapped && tok_is(p, p->pos, ")")) {
    size_t assignment_pos = p->pos;
    while (tok_is(p, assignment_pos, ")"))
      ++assignment_pos;
    if (is_assignment_operator(p, assignment_pos))
      p->pos = assignment_pos;
  }
  if (address.type == C_AGG) {
    if (read_only) {
      fail(p, "cannot assign to a const aggregate value");
      return false;
    }
    if (!expect(p, "="))
      return false;
    c_value_t value = parse_expr(p, 1);
    if (value.value < 0)
      return false;
    if (value.type != C_AGG || value.agg != address.agg) {
      fail(p, "aggregate assignment value type must match the target");
      return false;
    }
    if (!expect(p, ";"))
      return false;
    return lower_copy_struct(p, address.value, value.value, address.agg->size);
  }
  if (address.type == C_AGG_PTR) {
    if (read_only) {
      fail(p, "cannot assign through a pointer to const aggregate data");
      return false;
    }
    if (!expect(p, "="))
      return false;
    c_value_t value = parse_expr(p, 1);
    if (value.value < 0)
      return false;
    if (value.type == C_AGG) {
      bool compat_agg =
          value.agg == address.agg ||
          (getenv("NYC_KERNEL_COMPAT") &&
           ((value.agg && address.agg &&
             value.agg->size == address.agg->size) ||
            (!value.agg && address.agg) || (value.agg && !address.agg)));
      if (compat_agg) {
        const c_agg_type_t *copy_agg = address.agg ? address.agg : value.agg;
        if (!copy_agg || !copy_agg->size) {
          fail(p, "aggregate assignment has unknown size");
          return false;
        }
        if (!expect(p, ";"))
          return false;
        return lower_copy_struct(p, address.value, value.value, copy_agg->size);
      }
    }
    if (!c_type_is_pointer(value.type) &&
        !(c_type_is_integer(value.type) &&
          p->last_value_null_pointer_constant)) {
      fail(p, "aggregate pointer assignment requires a matching value or null");
      return false;
    }
    return (!wrapped || expect(p, ")")) && expect(p, ";") &&
           lower_store_pointer_field(p, address.value, value.value, false);
  }
  if (address.type != C_CHAR_PTR && address.type != C_I8_PTR &&
      address.type != C_I32_PTR && address.type != C_U32_PTR &&
      address.type != C_I64_PTR && address.type != C_U64_PTR &&
      address.type != C_I16_PTR && address.type != C_U16_PTR &&
      address.type != C_F32_PTR && address.type != C_F64_PTR &&
      address.type != C_U16_PTR_PTR && address.type != C_VOID_PTR &&
      address.type != C_OPAQUE_PTR && address.type != C_CHAR_PTR_PTR &&
      address.type != C_I8_PTR_PTR && address.type != C_FUNC_PTR &&
      address.type != C_FUNC_PTR_PTR && address.type != C_OPAQUE_PTR_PTR &&
      address.type != C_OPAQUE_PTR_PTR_PTR && address.type != C_AGG_PTR_PTR)
    return false;
  if (!is_assignment_operator(p, p->pos)) {
    (void)expect(p, "=");
    return false;
  }
  ny_ctok_t op = *current(p);
  bool simple = ny_ctok_eq(op, "=");
  ++p->pos;
  c_value_t value = parse_expr(p, 1);
  c_type_t element_type = address.type == C_CHAR_PTR  ? C_U8
                          : address.type == C_I8_PTR  ? C_CHAR
                          : address.type == C_I16_PTR ? C_I16
                          : address.type == C_U16_PTR ? C_U16
                          : address.type == C_I32_PTR ? C_I32
                          : address.type == C_U32_PTR ? C_U32
                          : address.type == C_U64_PTR ? C_U64
                          : address.type == C_F32_PTR ? C_F32
                          : address.type == C_F64_PTR ? C_F64
                                                      : C_I64;
  if ((element_type == C_F32 || element_type == C_F64) &&
      c_type_is_integer(value.type)) {
    if (value.type == C_U64) {
      const char *helper =
          element_type == C_F32 ? "__floatundisf" : "__floatundidf";
      nyr_inst_t call = {.op = NYIR_CALL,
                         .dst = -1,
                         .a = value.value,
                         .imm = 1,
                         .flags = NYIR_INST_F_EXTERN,
                         .effects = NYIR_EFFECT_CALL,
                         .scalar_class = c_float_scalar_class(element_type),
                         .symbol = helper};
      nyr_call_arg_class_set(call.arg_scalar_classes, 0, NYIR_SCALAR_I64_RAW);
      value.value = emit_value(p, call);
    } else {
      value.value = emit_value(
          p, (nyr_inst_t){.op = element_type == C_F32 ? NYIR_I64_TO_F32
                                                      : NYIR_I64_TO_F64,
                          .dst = -1,
                          .a = value.value,
                          .scalar_class = c_float_scalar_class(element_type)});
    }
    value.type = element_type;
    if (value.value < 0)
      return false;
  } else if ((element_type == C_F32 || element_type == C_F64) &&
             c_type_is_float(value.type) && value.type != element_type) {
    value.value = emit_value(
        p, (nyr_inst_t){.op = element_type == C_F32 ? NYIR_F64_TO_F32
                                                    : NYIR_F32_TO_F64,
                        .dst = -1,
                        .a = value.value,
                        .scalar_class = c_float_scalar_class(element_type)});
    value.type = element_type;
    if (value.value < 0)
      return false;
  }
  bool pointer_slot =
      address.type == C_VOID_PTR || address.type == C_OPAQUE_PTR ||
      address.type == C_CHAR_PTR_PTR || address.type == C_I8_PTR_PTR ||
      address.type == C_U16_PTR_PTR || address.type == C_FUNC_PTR ||
      address.type == C_FUNC_PTR_PTR || address.type == C_OPAQUE_PTR_PTR ||
      address.type == C_OPAQUE_PTR_PTR_PTR || address.type == C_AGG_PTR_PTR;
  bool pointer_add_assign =
      pointer_slot && (ny_ctok_eq(op, "+=") || ny_ctok_eq(op, "-=")) &&
      address.type != C_FUNC_PTR_PTR && address.type != C_VOID_PTR &&
      c_type_is_integer(value.type);
  bool valid_value =
      pointer_slot
          ? pointer_add_assign || c_type_is_pointer(value.type) ||
                (c_type_is_integer(value.type) &&
                 p->last_value_null_pointer_constant) ||
                (getenv("NYC_KERNEL_COMPAT") && c_type_is_integer(value.type))
      : (address.type == C_CHAR_PTR || address.type == C_I8_PTR ||
         address.type == C_I16_PTR || address.type == C_U16_PTR ||
         address.type == C_I32_PTR || address.type == C_U32_PTR)
          ? c_type_is_integer(value.type)
      : (address.type == C_I64_PTR || address.type == C_U64_PTR)
          ? c_type_is_integer(value.type)
          : value.type == element_type;
  if (!valid_value || value.value < 0) {
    if (!getenv("NYC_KERNEL_COMPAT")) {
      if (pointer_slot) {
        char message[128];
        snprintf(message, sizeof(message),
                 "pointer-array assignment requires a pointer or null "
                 "(target %d, value %d)",
                 address.type, value.type);
        fail(p, message);
      } else if (address.type == C_CHAR_PTR || address.type == C_I32_PTR ||
                 address.type == C_U32_PTR) {
        fail(p, "char pointer assignment requires an integer value");
      } else {
        char message[128];
        snprintf(message, sizeof(message),
                 "pointer assignment type mismatch: target %d, value %d",
                 address.type, value.type);
        fail(p, message);
      }
      return false;
    }
    if (value.value < 0)
      return false;
    /* NYC kernel compat: accept NYC mis-typed pointer/aggregate/integer
       assignments in kernel macro contexts. */
  }
  if (read_only && !pointer_slot) {
    fail(p, "cannot write through a pointer to const C data");
    return false;
  }
  if (address.type == C_I16_PTR || address.type == C_U16_PTR) {
    value.value = normalize_u16(p, value.value);
    if (value.value < 0)
      return false;
  }
  if (!simple) {
    int old_value =
        address.type == C_I16_PTR || address.type == C_U16_PTR
            ? lower_load_u16_field(p, address.value, element_type,
                                   c_value_pointee_volatile(address))
            : emit_value(
                  p,
                  (nyr_inst_t){
                      .op = NYIR_LOAD_I64,
                      .dst = -1,
                      .a = address.value,
                      .imm = 0,
                      .effects = NYIR_EFFECT_READ_MEMORY |
                                 NYIR_EFFECT_MAY_TRAP |
                                 (c_value_pointee_volatile(address)
                                      ? NYIR_EFFECT_VOLATILE
                                      : 0),
                      .flags =
                          address.type == C_CHAR_PTR || address.type == C_I8_PTR
                              ? NYIR_INST_F_MEM_BYTE
                          : (address.type == C_I16_PTR ||
                             address.type == C_U16_PTR)
                              ? 0
                              : (address.type == C_I32_PTR ||
                                         address.type == C_U32_PTR ||
                                         address.type == C_F32_PTR ||
                                         (p->abi == NYC_ABI_I386 &&
                                          pointer_slot)
                                     ? NYIR_INST_F_MEM_I32
                                     : 0),
                      .scalar_class = c_type_is_float(element_type)
                                          ? c_float_scalar_class(element_type)
                                          : NYIR_SCALAR_I64_RAW});
    if (old_value >= 0 && c_type_is_integer(element_type))
      old_value = normalize_integer(p, element_type, old_value);
    if (pointer_add_assign) {
      if (old_value < 0) {
        fail(p, "failed to load pointer for compound arithmetic");
        return false;
      }
      size_t scale =
          address.type == C_AGG_PTR_PTR && address.agg ? address.agg->size
          : address.type == C_CHAR_PTR_PTR || address.type == C_I8_PTR_PTR
              ? 1
              : c_type_size(p->abi, C_VOID_PTR);
      if (!scale || scale > INT64_MAX) {
        fail(p, "invalid pointer arithmetic stride");
        return false;
      }
      if (scale != 1) {
        int stride = emit_i64_constant(p, (int64_t)scale);
        value.value =
            stride < 0 ? -1
                       : emit_i64_binary(p, NYIR_MUL_I64, value.value, stride);
      }
      if (value.value < 0)
        return false;
      value.value =
          emit_value(p, (nyr_inst_t){.op = ny_ctok_eq(op, "-=") ? NYIR_SUB_I64
                                                                : NYIR_ADD_I64,
                                     .dst = -1,
                                     .a = old_value,
                                     .b = value.value,
                                     .scalar_class = NYIR_SCALAR_I64_RAW});
      if (value.value < 0)
        return false;
    } else {
      nyr_op_t ir_op = compound_assignment_ir(element_type, op);
      if (old_value < 0 || ir_op == NYIR_NOP) {
        fail(p, "compound pointer assignment operator is unsupported for type");
        return false;
      }
      value.value = emit_value(
          p,
          (nyr_inst_t){.op = ir_op,
                       .dst = -1,
                       .a = old_value,
                       .b = value.value,
                       .scalar_class = c_type_is_float(element_type)
                                           ? c_float_scalar_class(element_type)
                                           : NYIR_SCALAR_I64_RAW});
      if (value.value < 0)
        return false;
      if (address.type == C_I16_PTR || address.type == C_U16_PTR) {
        value.value = normalize_u16(p, value.value);
        if (value.value < 0)
          return false;
      }
    }
  }
  if (!expect(p, ";"))
    return false;
  if (address.type == C_I16_PTR || address.type == C_U16_PTR)
    return lower_store_u16_field(p, address.value, value.value, element_type,
                                 c_value_pointee_volatile(address));
  return emit(
      p,
      (nyr_inst_t){
          .op = NYIR_STORE_I64,
          .dst = -1,
          .a = address.value,
          .c = value.value,
          .imm = 0,
          .effects =
              NYIR_EFFECT_WRITE_MEMORY | NYIR_EFFECT_MAY_TRAP |
              (c_value_pointee_volatile(address) ? NYIR_EFFECT_VOLATILE : 0),
          .flags =
              address.type == C_CHAR_PTR || address.type == C_I8_PTR
                  ? NYIR_INST_F_MEM_BYTE
              : address.type == C_I16_PTR || address.type == C_U16_PTR
                  ? 0
                  : (address.type == C_I32_PTR || address.type == C_U32_PTR ||
                             address.type == C_F32_PTR ||
                             (p->abi == NYC_ABI_I386 && pointer_slot)
                         ? NYIR_INST_F_MEM_I32
                         : 0),
          .scalar_class = c_type_is_float(element_type)
                              ? c_float_scalar_class(element_type)
                              : NYIR_SCALAR_I64_RAW},
      NULL);
}

static bool lower_global_assignment(c_parser_t *p, c_global_t *global,
                                    bool semicolon) {
  if (global_register_use_fails(p, global))
    return false;
  if (global->is_array ||
      (global->is_const && !c_type_is_pointer(global->type))) {
    fail(p, "file-scope C object is not writable by this frontend");
    return false;
  }
  ny_ctok_t assignment = p->tokens[p->pos + 1];
  bool simple = ny_ctok_eq(assignment, "=");
  bool pointer_compound =
      c_type_is_arithmetic_pointer(global->type) &&
      (ny_ctok_eq(assignment, "+=") || ny_ctok_eq(assignment, "-="));
  if (!simple && !c_type_is_integer(global->type) && !pointer_compound) {
    fail(p, "compound assignment to this file-scope C type is unsupported");
    return false;
  }
  int address = -1;
  int old_value = -1;
  bool aggregate_assignment = simple && global->type == C_AGG && global->agg;
  if (aggregate_assignment) {
    static const char data_prefix[] = "__nyc_data_";
    static const char extern_prefix[] = "__nyc_extern_";
    const char *prefix =
        global->is_external || global->is_weak ? extern_prefix : data_prefix;
    size_t prefix_len = strlen(prefix);
    size_t symbol_len = prefix_len + global->name.len;
    char *symbol = malloc(symbol_len + 1);
    if (!symbol) {
      fail(p, "out of memory lowering a file-scope aggregate assignment");
      return false;
    }
    memcpy(symbol, prefix, prefix_len);
    memcpy(symbol + prefix_len, global->name.start, global->name.len);
    symbol[symbol_len] = '\0';
    address = emit_value(
        p, (nyr_inst_t){.op = NYIR_ADDR_SYMBOL, .dst = -1, .symbol = symbol});
    free(symbol);
    if (address < 0)
      return false;
  }
  if (!simple) {
    static const char data_prefix[] = "__nyc_data_";
    static const char extern_prefix[] = "__nyc_extern_";
    const char *prefix =
        global->is_external || global->is_weak ? extern_prefix : data_prefix;
    size_t prefix_len = strlen(prefix);
    size_t symbol_len = prefix_len + global->name.len;
    char *symbol = malloc(symbol_len + 1);
    if (!symbol) {
      fail(p, "out of memory lowering a file-scope C compound assignment");
      return false;
    }
    memcpy(symbol, prefix, prefix_len);
    memcpy(symbol + prefix_len, global->name.start, global->name.len);
    symbol[symbol_len] = '\0';
    address = emit_value(
        p, (nyr_inst_t){.op = NYIR_ADDR_SYMBOL, .dst = -1, .symbol = symbol});
    free(symbol);
    uint8_t memory_flags = global->type == C_I32 || global->type == C_U32
                               ? NYIR_INST_F_MEM_I32
                               : (global->type == C_U8 || global->type == C_CHAR
                                      ? NYIR_INST_F_MEM_BYTE
                                      : 0);
    if (address >= 0)
      old_value =
          emit_value(p, (nyr_inst_t){.op = NYIR_LOAD_I64,
                                     .dst = -1,
                                     .a = address,
                                     .effects = NYIR_EFFECT_READ_MEMORY |
                                                NYIR_EFFECT_MAY_TRAP,
                                     .flags = memory_flags,
                                     .scalar_class = NYIR_SCALAR_I64_RAW});
    if (old_value < 0)
      return false;
    old_value = normalize_integer(p, global->type, old_value);
    if (old_value < 0)
      return false;
  }
  p->pos += 2;
  c_value_t value = parse_expr(p, 1);
  if (aggregate_assignment) {
    if (value.value < 0)
      return false;
    if (value.type != C_AGG || value.agg != global->agg) {
      fail(p, "global aggregate assignment has incompatible type");
      return false;
    }
    if (semicolon && !expect(p, ";"))
      return false;
    return lower_copy_struct(p, address, value.value, global->agg->size);
  }
  bool valid =
      global->type == C_F64             ? value.type == C_F64
      : c_type_is_integer(global->type) ? c_type_is_integer(value.type)
      : c_type_is_pointer(global->type)
          ? c_type_is_pointer(value.type) || c_type_is_integer(value.type)
          : false;
  if (!simple)
    valid =
        pointer_compound
            ? c_type_is_integer(value.type)
            : c_type_is_integer(value.type) &&
                  compound_assignment_ir(global->type, assignment) != NYIR_NOP;
  if (value.value < 0 || !valid) {
    if (value.value >= 0)
      fail(p, "global initializer assignment has incompatible type");
    return false;
  }
  if (!simple) {
    if (pointer_compound) {
      value.value =
          lower_pointer_offset_for(p, global->type, global->agg, old_value,
                                   value.value, ny_ctok_eq(assignment, "-="));
      if (value.value < 0)
        return false;
      value.type = global->type;
    } else {
      nyr_op_t ir_op = compound_assignment_ir(global->type, assignment);
      bool unsigned_type = global->type == C_U32 || global->type == C_U64;
      if (global->type == C_U32)
        value.value = normalize_u32(p, value.value);
      if (value.value < 0)
        return false;
      if (unsigned_type &&
          (ny_ctok_eq(assignment, "/=") || ny_ctok_eq(assignment, "%="))) {
        bool wide = global->type == C_U64;
        value.value = emit_value(
            p, (nyr_inst_t){.op = NYIR_INLINE_ASM,
                            .dst = -1,
                            .a = old_value,
                            .b = value.value,
                            .imm = ny_ctok_eq(assignment, "/=") ? -21 : -22,
                            .effects = NYIR_EFFECT_VOLATILE,
                            .symbol = "unsigned-div"});
        if (!wide && value.value >= 0)
          value.value = normalize_u32(p, value.value);
      } else if (unsigned_type && ny_ctok_eq(assignment, ">>=")) {
        if (global->type == C_U32) {
          /*
           * U32 values are zero-extended before entering NYIR, so arithmetic
           * right shift has the same result as logical shift for this width.
           */
          ir_op = NYIR_SAR_I64;
        } else {
          value.value =
              emit_value(p, (nyr_inst_t){.op = NYIR_INLINE_ASM,
                                         .dst = -1,
                                         .a = old_value,
                                         .b = value.value,
                                         .imm = -23,
                                         .effects = NYIR_EFFECT_NONE,
                                         .symbol = "unsigned-shift-right"});
        }
      } else {
        value.value =
            emit_value(p, (nyr_inst_t){.op = ir_op,
                                       .dst = -1,
                                       .a = old_value,
                                       .b = value.value,
                                       .scalar_class = NYIR_SCALAR_I64_RAW});
      }
    }
    if (value.value < 0)
      return false;
    if (!pointer_compound) {
      value.value = normalize_integer(p, global->type, value.value);
      if (value.value < 0)
        return false;
    }
  }
  if (global->type == C_I32 || global->type == C_U32 || global->type == C_U8) {
    value.value = normalize_integer(p, global->type, value.value);
    if (value.value < 0)
      return false;
  }
  if (semicolon && !expect(p, ";"))
    return false;
  if (address < 0) {
    static const char data_prefix[] = "__nyc_data_";
    static const char extern_prefix[] = "__nyc_extern_";
    const char *prefix =
        global->is_external || global->is_weak ? extern_prefix : data_prefix;
    size_t prefix_len = strlen(prefix);
    size_t symbol_len = prefix_len + global->name.len;
    char *symbol = malloc(symbol_len + 1);
    if (!symbol) {
      fail(p, "out of memory lowering a file-scope C assignment");
      return false;
    }
    memcpy(symbol, prefix, prefix_len);
    memcpy(symbol + prefix_len, global->name.start, global->name.len);
    symbol[symbol_len] = '\0';
    address = emit_value(
        p, (nyr_inst_t){.op = NYIR_ADDR_SYMBOL, .dst = -1, .symbol = symbol});
    free(symbol);
  }
  if (address < 0)
    return false;
  uint8_t flags = global->type == C_I32 || global->type == C_U32
                      ? NYIR_INST_F_MEM_I32
                      : (global->type == C_U8 ? NYIR_INST_F_MEM_BYTE : 0);
  return emit(
      p,
      (nyr_inst_t){.op = NYIR_STORE_I64,
                   .dst = -1,
                   .a = address,
                   .c = value.value,
                   .effects = NYIR_EFFECT_WRITE_MEMORY | NYIR_EFFECT_MAY_TRAP,
                   .flags = flags,
                   .scalar_class = global->type == C_F64 ? NYIR_SCALAR_F64
                                                         : NYIR_SCALAR_I64_RAW},
      NULL);
}
