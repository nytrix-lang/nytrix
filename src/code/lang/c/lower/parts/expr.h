/* C expression parsing and value lowering, included by parse.c. */
static c_value_t parse_indirect_call(c_parser_t *p, c_value_t target);
static c_value_t parse_unary(c_parser_t *p);
static int lower_increment(c_parser_t *p, c_local_t *local, bool decrement,
                           bool prefix, int previous_value);
static int lower_increment_lvalue(c_parser_t *p, const c_lvalue_t *lvalue,
                                  bool decrement, bool prefix,
                                  int previous_value);
static size_t statement_end(c_parser_t *p, size_t at);

/*
 * Address of `name.field...` starting from an aggregate pointer. Produces
 * the field address for aggregate fields and loads scalar fields.
 */
static bool lower_member_rvalue(c_parser_t *p, c_value_t base, c_value_t *out) {
  c_value_t value = base;
  for (;;) {
    bool arrow = accept(p, "->");
    bool dot = !arrow && accept(p, ".");
    if (!dot && !arrow)
      break;
    if (dot && value.type != C_AGG) {
      char message[128];
      snprintf(message, sizeof(message),
               "'.' member access requires a struct or union value (type %d, "
               "aggregate fields %zu)",
               value.type, value.agg ? value.agg->field_count : 0);
      fail(p, message);
      return false;
    }
    if (arrow && value.type != C_AGG_PTR && value.type != C_AGG_ARRAY_PTR) {
      char message[128];
      snprintf(message, sizeof(message),
               "'->' member access requires an aggregate pointer (type %d)",
               value.type);
      fail(p, message);
      return false;
    }
    ny_ctok_t field_name;
    if (!parse_name(p, &field_name))
      return false;
    const c_agg_type_t *agg = value.agg;
    if (agg && !agg->complete) {
      for (size_t i = p->tag_count; i > 0; --i) {
        c_agg_type_t *candidate = p->tags[i - 1];
        if (candidate->complete && candidate->is_union == agg->is_union &&
            tok_same(candidate->name, agg->name)) {
          agg = candidate;
          value.agg = candidate;
          break;
        }
      }
    }
    if ((!agg || (!agg->complete && !agg->name.len)) &&
        value.type == C_AGG_PTR && field_name.len == 4 &&
        !memcmp(field_name.start, "bits", 4)) {
      c_agg_type_t *match = NULL;
      bool ambiguous = false;
      for (size_t i = 0; i < p->tag_count; ++i) {
        c_agg_type_t *candidate = p->tags[i];
        const c_agg_field_t *candidate_field =
            candidate->complete ? find_field(candidate, field_name) : NULL;
        if (!candidate_field || !candidate_field->is_array ||
            !candidate->name.len)
          continue;
        if (match && match != candidate) {
          ambiguous = true;
          break;
        }
        match = candidate;
      }
      if (match && !ambiguous) {
        agg = match;
        value.agg = match;
      }
    }
    if (!agg || !agg->complete) {
      char message[160];
      snprintf(message, sizeof(message),
               "member `%.*s` accessed through incomplete aggregate `%.*s` "
               "(%zu fields)",
               (int)(field_name.len > 96 ? 96 : field_name.len),
               field_name.start,
               agg && agg->name.len > 32 ? 32
               : agg                     ? (int)agg->name.len
                                         : 0,
               agg ? agg->name.start : "", agg ? agg->field_count : 0);
      fail(p, message);
      return false;
    }
    const c_agg_field_t *field = find_field(agg, field_name);
    if (!field) {
      char message[160];
      snprintf(message, sizeof(message),
               "struct/union has no such member `%.*s` in `%.*s` (%zu fields)",
               (int)(field_name.len > 48 ? 48 : field_name.len),
               field_name.start, (int)(agg->name.len > 48 ? 48 : agg->name.len),
               agg->name.start, agg->field_count);
      fail(p, message);
      return false;
    }
    p->last_type_bool = field->is_bool;
    p->last_value_pointee_const =
        field->kind == C_AGG_FIELD_PTR && field->pointee_const;
    if (field->is_function_pointer) {
      p->last_type_function_result_type = (c_type_t)field->function_result_type;
      p->last_type_function_result_agg =
          (c_agg_type_t *)field->function_result_agg;
      p->last_type_function_void_params = false;
      p->last_type_function_type = true;
      p->indirect_bool_params = 0; /* set below only for sources that know it */
      p->indirect_result_type = (c_type_t)field->function_result_type;
      p->indirect_result_agg = (c_agg_type_t *)field->function_result_agg;
      p->has_indirect_result_signature = true;
      p->indirect_bool_params = field->function_bool_params;
    }
    int address = value.value;
    if (field->offset)
      address = lower_add_const(p, address, field->offset);
    if (address < 0)
      return false;
    if (field->kind == C_AGG_FIELD_AGG) {
      /*
       * A field declared against a forward-declared tag keeps that
       * incomplete object; resolve it to the completed definition before
       * the aggregate is used in an initializer or assignment.
       */
      c_agg_type_t *field_agg = complete_tag(p, (c_agg_type_t *)field->agg);
      if (!field_agg)
        field_agg = (c_agg_type_t *)field->agg;
      if (!record_lvalue(p, address, address, C_AGG, field->is_volatile))
        return false;
      p->lvalues[p->lvalue_count - 1].agg = field_agg;
      value = (c_value_t){address, C_AGG, field_agg};
      continue;
    }
    if (field->kind == C_AGG_FIELD_ARRAY) {
      if (!accept(p, "[")) {
        size_t array_size = field->elem_size;
        for (unsigned dimension = 0; dimension < field->array_rank;
             ++dimension) {
          if (!field->array_extent[dimension] ||
              array_size > SIZE_MAX / field->array_extent[dimension]) {
            array_size = 0;
            break;
          }
          array_size *= field->array_extent[dimension];
        }
        p->last_type_is_array = true;
        p->last_type_array_decayed = false;
        p->last_type_array_size = array_size;
        c_type_t ptr_type =
            field->elem_pointer
                ? (field->array_rank == 1 &&
                           field->pointer_element_type != C_BAD
                       ? c_add_pointer_levels(
                             (c_type_t)field->pointer_element_type, 1)
                       : C_OPAQUE_PTR)
            : field->agg      ? C_AGG_PTR
            : field->elem_f32 ? C_F32_PTR
            : field->elem_f64 ? C_F64_PTR
            : field->elem_size == 1
                ? (field->is_signed_byte ? C_I8_PTR : C_CHAR_PTR)
            : field->elem_size == 2
                ? (field->elem_unsigned ? C_U16_PTR : C_I16_PTR)
            : field->elem_size == 4
                ? (field->elem_unsigned ? C_U32_PTR : C_I32_PTR)
            : field->elem_unsigned ? C_U64_PTR
                                   : C_I64_PTR;
        if (field->array_rank > 1) {
          c_array_shape_t shape = {
              .rank = field->array_rank,
              .element_size = field->elem_size,
              .element_type = field->elem_pointer
                                  ? (field->pointer_element_type != C_BAD
                                         ? (c_type_t)field->pointer_element_type
                                     : field->agg ? C_AGG_PTR
                                                  : C_OPAQUE_PTR)
                              : field->agg      ? C_AGG
                              : field->elem_f32 ? C_F32
                              : field->elem_f64 ? C_F64
                              : field->elem_size == 1
                                  ? (field->is_signed_byte ? C_CHAR : C_U8)
                              : field->elem_size == 2
                                  ? (field->elem_unsigned ? C_U16 : C_I16)
                              : field->elem_size == 4
                                  ? (field->elem_unsigned ? C_U32 : C_I32)
                              : field->elem_unsigned ? C_U64
                                                     : C_I64,
              .agg = field->agg};
          memcpy(shape.extent, field->array_extent,
                 field->array_rank * sizeof(shape.extent[0]));
          if (!remember_array_shape(p, address, shape))
            return false;
        }
        p->last_value_array = true;
        value = (c_value_t){address, ptr_type, field->agg};
        continue;
      }
      c_value_t index = parse_expr(p, 1);
      if (index.value < 0 || !c_type_is_integer(index.type) ||
          !expect(p, "]")) {
        fail(p, "array index must be an integer");
        return false;
      }
      size_t element_stride = field->elem_size;
      for (unsigned dimension = 1; dimension < field->array_rank; ++dimension) {
        if (field->array_extent[dimension] &&
            element_stride > SIZE_MAX / field->array_extent[dimension]) {
          fail(p, "aggregate array row stride overflows");
          return false;
        }
        element_stride *= field->array_extent[dimension];
      }
      int offset = index.value;
      if (element_stride != 1) {
        int stride =
            emit_value(p, (nyr_inst_t){.op = NYIR_CONST_I64,
                                       .dst = -1,
                                       .imm = (int64_t)element_stride});
        if (stride < 0)
          return false;
        offset =
            emit_value(p, (nyr_inst_t){.op = NYIR_MUL_I64,
                                       .dst = -1,
                                       .a = index.value,
                                       .b = stride,
                                       .flags = c_pointer_offset_flags(p)});
        if (offset < 0)
          return false;
      }
      address = emit_value(p, (nyr_inst_t){.op = NYIR_ADD_I64,
                                           .dst = -1,
                                           .a = address,
                                           .b = offset,
                                           .flags = c_pointer_offset_flags(p)});
      if (address < 0)
        return false;
      if (field->array_rank > 1) {
        c_type_t pointer_type =
            field->agg && !field->elem_pointer ? C_AGG_PTR
            : field->elem_pointer ? (field->pointer_element_type != C_BAD
                                         ? (c_type_t)field->pointer_element_type
                                     : field->agg ? C_AGG_PTR
                                                  : C_OPAQUE_PTR)
            : field->elem_f32     ? C_F32_PTR
            : field->elem_f64     ? C_F64_PTR
            : field->elem_size == 1
                ? (field->is_signed_byte ? C_I8_PTR : C_CHAR_PTR)
            : field->elem_size == 2
                ? (field->elem_unsigned ? C_U16_PTR : C_I16_PTR)
            : field->elem_size == 4
                ? (field->elem_unsigned ? C_U32_PTR : C_I32_PTR)
            : field->elem_unsigned ? C_U64_PTR
                                   : C_I64_PTR;
        c_array_shape_t shape = {
            .rank = field->array_rank - 1,
            .element_size = field->elem_size,
            .element_type =
                field->elem_pointer
                    ? (field->pointer_element_type != C_BAD
                           ? (c_type_t)field->pointer_element_type
                       : field->agg ? C_AGG_PTR
                                    : C_OPAQUE_PTR)
                : field->agg      ? C_AGG
                : field->elem_f32 ? C_F32
                : field->elem_f64 ? C_F64
                : field->elem_size == 1
                    ? (field->is_signed_byte ? C_CHAR : C_U8)
                : field->elem_size == 2 ? (field->elem_unsigned ? C_U16 : C_I16)
                : field->elem_size == 4 ? (field->elem_unsigned ? C_U32 : C_I32)
                : field->elem_unsigned  ? C_U64
                                        : C_I64,
            .agg = field->agg};
        memcpy(shape.extent, field->array_extent + 1,
               shape.rank * sizeof(shape.extent[0]));
        if (!remember_array_shape(p, address, shape))
          return false;
        value = (c_value_t){address, pointer_type, field->agg};
        continue;
      }
      if (field->agg && !field->elem_pointer) {
        if (!record_lvalue(p, address, address, C_AGG, field->is_volatile))
          return false;
        p->lvalues[p->lvalue_count - 1].agg = field->agg;
        value = (c_value_t){address, C_AGG, field->agg};
        continue;
      }
      bool is_byte = field->elem_size == 1;
      bool is_short = field->elem_size == 2;
      bool is_i32_element = field->elem_size == 4;
      int loaded =
          is_short
              ? lower_load_u16_field(p, address,
                                     field->elem_unsigned ? C_U16 : C_I16,
                                     field->is_volatile)
              : emit_value(
                    p,
                    (nyr_inst_t){.op = NYIR_LOAD_I64,
                                 .dst = -1,
                                 .a = address,
                                 .effects = NYIR_EFFECT_READ_MEMORY |
                                            NYIR_EFFECT_MAY_TRAP,
                                 .flags = is_byte ? NYIR_INST_F_MEM_BYTE
                                          : is_i32_element ? NYIR_INST_F_MEM_I32
                                                           : 0,
                                 .scalar_class =
                                     c_type_is_float(field->elem_f32   ? C_F32
                                                     : field->elem_f64 ? C_F64
                                                                       : C_BAD)
                                         ? c_float_scalar_class(
                                               field->elem_f32 ? C_F32 : C_F64)
                                         : NYIR_SCALAR_I64_RAW});
      if (loaded < 0)
        return false;
      c_type_t elem_type =
          field->elem_pointer ? (field->pointer_element_type != C_BAD
                                     ? (c_type_t)field->pointer_element_type
                                 : field->agg ? C_AGG_PTR
                                              : C_OPAQUE_PTR)
          : field->elem_f32   ? C_F32
          : field->elem_f64   ? C_F64
          : is_byte           ? (field->is_signed_byte ? C_CHAR : C_U8)
          : is_short          ? (field->elem_unsigned ? C_U16 : C_I16)
          : is_i32_element    ? (field->elem_unsigned ? C_U32 : C_I32)
                              : (field->elem_unsigned ? C_U64 : C_I64);
      if (loaded >= 0 && c_type_is_integer(elem_type))
        loaded = normalize_integer(p, elem_type, loaded);
      if (loaded < 0 ||
          !record_lvalue(p, loaded, address, elem_type, field->is_volatile))
        return false;
      value = (c_value_t){loaded, elem_type,
                          field->elem_pointer ? field->agg : NULL};
      continue;
    }
    if (field->kind == C_AGG_FIELD_PTR) {
      int loaded = lower_load_pointer_field(p, address, field->is_volatile);
      if (loaded < 0)
        return false;
      if (field->pointer_to_array && !field->pointer_to_aggregate_array) {
        c_type_t element_type = field->pointer_element_type != C_BAD
                                    ? (c_type_t)field->pointer_element_type
                                    : C_BAD;
        size_t element_size = element_type == C_AGG && field->agg
                                  ? field->agg->size
                                  : c_type_size(p->abi, element_type);
        if (!field->array_rank || !element_size) {
          fail(p, "pointer-to-array field has an incomplete element type");
          return false;
        }
        c_array_shape_t shape = {.rank = field->array_rank,
                                 .element_type = element_type,
                                 .element_size = element_size,
                                 .agg = field->agg,
                                 .pointer_to_array = true};
        memcpy(shape.extent, field->array_extent,
               field->array_rank * sizeof(shape.extent[0]));
        if (!remember_array_shape(p, loaded, shape))
          return false;
      }
      if (field->pointer_to_aggregate_array && accept(p, "[")) {
        c_value_t index = parse_expr(p, 1);
        if (index.value < 0 || !c_type_is_integer(index.type) ||
            !expect(p, "]") || !field->agg || !field->elem_size) {
          fail(p, "aggregate-array pointer index requires a sized target");
          return false;
        }
        int stride =
            emit_value(p, (nyr_inst_t){.op = NYIR_CONST_I64,
                                       .dst = -1,
                                       .imm = (int64_t)field->elem_size});
        int offset =
            stride < 0
                ? -1
                : emit_value(p,
                             (nyr_inst_t){.op = NYIR_MUL_I64,
                                          .dst = -1,
                                          .a = index.value,
                                          .b = stride,
                                          .flags = c_pointer_offset_flags(p)});
        address =
            offset < 0
                ? -1
                : emit_value(p,
                             (nyr_inst_t){.op = NYIR_ADD_I64,
                                          .dst = -1,
                                          .a = loaded,
                                          .b = offset,
                                          .flags = c_pointer_offset_flags(p)});
        if (address < 0 ||
            !record_lvalue(p, address, address, C_AGG_PTR, field->is_volatile))
          return false;
        p->lvalues[p->lvalue_count - 1].agg = field->agg;
        p->last_value_array = true;
        value = (c_value_t){address, C_AGG_PTR, field->agg};
        continue;
      }
      if (field->agg && accept(p, "[")) {
        c_value_t index = parse_expr(p, 1);
        bool pointer_element = field->pointer_depth > 1;
        if (index.value < 0 || !c_type_is_integer(index.type) ||
            !expect(p, "]") || (!pointer_element && !field->agg->complete)) {
          fail(p, "aggregate pointer index requires an integer and complete "
                  "target type");
          return false;
        }
        int stride = emit_value(
            p,
            (nyr_inst_t){.op = NYIR_CONST_I64,
                         .dst = -1,
                         .imm = pointer_element
                                    ? (int64_t)c_type_size(p->abi, C_VOID_PTR)
                                    : (int64_t)field->agg->size});
        int offset =
            stride < 0
                ? -1
                : emit_value(p,
                             (nyr_inst_t){.op = NYIR_MUL_I64,
                                          .dst = -1,
                                          .a = index.value,
                                          .b = stride,
                                          .flags = c_pointer_offset_flags(p)});
        address =
            offset < 0
                ? -1
                : emit_value(p,
                             (nyr_inst_t){.op = NYIR_ADD_I64,
                                          .dst = -1,
                                          .a = loaded,
                                          .b = offset,
                                          .flags = c_pointer_offset_flags(p)});
        if (address < 0)
          return false;
        if (pointer_element) {
          c_type_t element_type =
              field->pointer_depth > 2 ? C_OPAQUE_PTR_PTR : C_AGG_PTR;
          int element =
              lower_load_pointer_field(p, address, field->is_volatile);
          if (element < 0 || !record_lvalue(p, element, address, element_type,
                                            field->is_volatile))
            return false;
          p->lvalues[p->lvalue_count - 1].agg = field->agg;
          value = (c_value_t){element, element_type, field->agg};
        } else {
          if (!record_lvalue(p, address, address, C_AGG, field->is_volatile))
            return false;
          p->lvalues[p->lvalue_count - 1].agg = field->agg;
          value = (c_value_t){address, C_AGG, field->agg};
        }
        continue;
      }
      if (accept(p, "[")) {
        c_value_t index = parse_expr(p, 1);
        size_t stride = field->is_function_pointer || field->elem_pointer
                            ? c_type_size(p->abi, C_FUNC_PTR)
                            : field->elem_size;
        if (index.value < 0 || !c_type_is_integer(index.type) ||
            !expect(p, "]") || !stride) {
          fail(p, "pointer field index requires an integer and sized element");
          return false;
        }
        int offset = index.value;
        if (stride > 1) {
          int scale = emit_value(p, (nyr_inst_t){.op = NYIR_CONST_I64,
                                                 .dst = -1,
                                                 .imm = (int64_t)stride});
          offset = scale < 0
                       ? -1
                       : emit_value(p, (nyr_inst_t){
                                           .op = NYIR_MUL_I64,
                                           .dst = -1,
                                           .a = index.value,
                                           .b = scale,
                                           .flags = c_pointer_offset_flags(p)});
        }
        address = offset < 0 ? -1
                             : emit_value(p, (nyr_inst_t){.op = NYIR_ADD_I64,
                                                          .dst = -1,
                                                          .a = loaded,
                                                          .b = offset});
        /*
         * Indexing a pointer-to-array member yields the row, which decays to
         * a pointer to its first element; it must not load a scalar from the
         * row. Record the row shape so a second subscript still works.
         */
        if (field->pointer_to_array && address >= 0) {
          c_type_t element_type = (c_type_t)field->pointer_element_type;
          size_t element_size = c_type_size(p->abi, element_type);
          if (!element_size) {
            fail(p, "pointer-to-array field has an incomplete element type");
            return false;
          }
          c_array_shape_t shape = {.rank = 1,
                                   .element_size = element_size,
                                   .element_type = element_type,
                                   .pointer_to_array = true};
          shape.extent[0] = field->elem_size / element_size;
          if (!shape.extent[0] || !remember_array_shape(p, address, shape))
            return false;
          c_type_t row_type = c_add_pointer_levels(element_type, 1);
          if (row_type == C_BAD ||
              !record_lvalue(p, address, address, row_type, field->is_volatile))
            return false;
          value = (c_value_t){address, row_type, field->agg};
          continue;
        }
        int element =
            address < 0 ? -1
            : stride == 2
                ? lower_load_u16_field(p, address,
                                       field->elem_unsigned ? C_U16 : C_I16,
                                       field->is_volatile)
                : emit_value(p,
                             (nyr_inst_t){
                                 .op = NYIR_LOAD_I64,
                                 .dst = -1,
                                 .a = address,
                                 .effects = NYIR_EFFECT_READ_MEMORY |
                                            NYIR_EFFECT_MAY_TRAP,
                                 .flags = stride == 1   ? NYIR_INST_F_MEM_BYTE
                                          : stride == 4 ? NYIR_INST_F_MEM_I32
                                                        : 0,
                                 .scalar_class =
                                     field->elem_f32 || field->elem_f64
                                         ? c_float_scalar_class(
                                               field->elem_f32 ? C_F32 : C_F64)
                                         : NYIR_SCALAR_I64_RAW});
        if (element < 0)
          return false;
        c_type_t element_type =
            field->pointer_element_type != C_BAD
                ? (c_type_t)field->pointer_element_type
            : field->is_function_pointer || field->elem_function_pointer
                ? C_FUNC_PTR
            : field->agg          ? C_AGG_PTR
            : field->elem_pointer ? C_OPAQUE_PTR
            : field->elem_f32     ? C_F32
            : field->elem_f64     ? C_F64
            : stride == 1         ? (field->is_signed_byte ? C_CHAR : C_U8)
            : stride == 2         ? (field->elem_unsigned ? C_U16 : C_I16)
            : stride == 4         ? (field->elem_unsigned ? C_U32 : C_I32)
                                  : (field->elem_unsigned ? C_U64 : C_I64);
        if (field->has_function_signature) {
          p->indirect_bool_params = 0; /* set below only for sources that know it */
          p->indirect_result_type = (c_type_t)field->function_result_type;
          p->indirect_result_agg = field->function_result_agg;
          p->has_indirect_result_signature = true;
          p->indirect_bool_params = field->function_bool_params;
        }
        if (c_type_is_integer(element_type))
          element = normalize_integer(p, element_type, element);
        if (element < 0 || !record_lvalue(p, element, address, element_type,
                                          field->is_volatile))
          return false;
        value = (c_value_t){element, element_type, field->agg};
        continue;
      }
      c_type_t pointer_type =
          field->pointer_to_aggregate_array ? C_AGG_ARRAY_PTR
          : field->pointer_element_type != C_BAD && field->pointer_depth
              ? c_add_pointer_levels((c_type_t)field->pointer_element_type, 1)
          : field->agg && field->pointer_depth > 2   ? C_OPAQUE_PTR_PTR_PTR
          : field->agg && field->pointer_depth > 1   ? C_AGG_PTR_PTR
          : field->agg                               ? C_AGG_PTR
          : field->pointer_element_type == C_VA_LIST ? C_VA_LIST_PTR
          : field->elem_function_pointer             ? C_FUNC_PTR_PTR
          : field->is_function_pointer               ? C_FUNC_PTR
          : field->elem_char_pointer                 ? C_CHAR_PTR_PTR
          : field->pointer_depth == 1 && field->pointer_element_type == C_BAD
              ? C_VOID_PTR
          : field->elem_pointer ? C_OPAQUE_PTR_PTR
          : field->elem_f32     ? C_F32_PTR
          : field->elem_f64     ? C_F64_PTR
          : field->elem_size == 1
              ? (field->is_signed_byte ? C_I8_PTR : C_CHAR_PTR)
          : field->elem_size == 2
              ? (field->elem_unsigned ? C_U16_PTR : C_I16_PTR)
          : field->elem_size == 4
              ? (field->elem_unsigned ? C_U32_PTR : C_I32_PTR)
          : field->elem_unsigned ? C_U64_PTR
                                 : C_I64_PTR;
      if (!record_lvalue(p, loaded, address, pointer_type, field->is_volatile))
        return false;
      p->lvalues[p->lvalue_count - 1].agg = field->agg;
      if (field->has_function_signature) {
        p->indirect_bool_params = 0; /* set below only for sources that know it */
        p->indirect_result_type = (c_type_t)field->function_result_type;
        p->indirect_result_agg = field->function_result_agg;
        p->has_indirect_result_signature = true;
        p->indirect_bool_params = field->function_bool_params;
      }
      value = (c_value_t){loaded, pointer_type, field->agg};
      continue;
    }
    if (field->is_bitfield) {
      int loaded = lower_load_bitfield(p, address, field);
      c_type_t type = field->bit_width < 32            ? C_I32
                      : field->kind == C_AGG_FIELD_I32 ? C_I32
                      : field->kind == C_AGG_FIELD_I64 ? C_I64
                      : field->kind == C_AGG_FIELD_U64 ? C_U64
                                                       : C_U32;
      if (loaded < 0 ||
          !record_lvalue(p, loaded, address, type, field->is_volatile))
        return false;
      p->lvalues[p->lvalue_count - 1].bitfield = field;
      *out = (c_value_t){loaded, type, NULL};
      return true;
    }
    if (field->kind == C_AGG_FIELD_C64 || field->kind == C_AGG_FIELD_C128) {
      /*
       * A complex member rvalue is its address tagged with a synthetic
       * two-field aggregate: the existing aggregate copy path moves both
       * halves, arithmetic keeps rejecting C_AGG instead of using the
       * address, and component access recognizes the SIZE_MAX marker.
       */
      static const c_agg_type_t complex_f32_agg = {
          .scope_id = SIZE_MAX,
          .complete = true,
          .size = 8,
          .align = 4,
          .field_count = 2,
          .fields = {
              {.kind = C_AGG_FIELD_F32, .offset = 0, .size = 4, .align = 4},
              {.kind = C_AGG_FIELD_F32, .offset = 4, .size = 4, .align = 4}}};
      static const c_agg_type_t complex_f64_agg = {
          .scope_id = SIZE_MAX,
          .complete = true,
          .size = 16,
          .align = 8,
          .field_count = 2,
          .fields = {
              {.kind = C_AGG_FIELD_F64, .offset = 0, .size = 8, .align = 8},
              {.kind = C_AGG_FIELD_F64, .offset = 8, .size = 8, .align = 8}}};
      const c_agg_type_t *complex_agg =
          field->kind == C_AGG_FIELD_C64 ? &complex_f32_agg : &complex_f64_agg;
      if (!record_lvalue(p, address, address, C_AGG, field->is_volatile))
        return false;
      p->lvalues[p->lvalue_count - 1].agg = complex_agg;
      *out = (c_value_t){address, C_AGG, complex_agg};
      return true;
    }
    bool is_f32 = field->kind == C_AGG_FIELD_F32;
    bool is_f64 = field->kind == C_AGG_FIELD_F64;
    bool is_i32 = field->kind == C_AGG_FIELD_I32;
    bool is_i16 = field->kind == C_AGG_FIELD_I16;
    bool is_u8 = field->kind == C_AGG_FIELD_U8;
    bool is_u32 = field->kind == C_AGG_FIELD_U32;
    bool is_u16 = field->kind == C_AGG_FIELD_U16;
    bool is_u64 = field->kind == C_AGG_FIELD_U64;
    c_type_t field_type = is_i16 ? C_I16 : is_u16 ? C_U16 : C_BAD;
    int loaded =
        is_f32
            ? emit_value(p, (nyr_inst_t){.op = NYIR_LOAD_I64,
                                         .dst = -1,
                                         .a = address,
                                         .effects = NYIR_EFFECT_READ_MEMORY |
                                                    NYIR_EFFECT_MAY_TRAP |
                                                    (field->is_volatile
                                                         ? NYIR_EFFECT_VOLATILE
                                                         : 0),
                                         .flags = NYIR_INST_F_MEM_I32,
                                         .scalar_class = NYIR_SCALAR_F32})
        : is_i32 ? lower_load_i32_field(p, address, field->is_volatile)
        : is_u32
            ? emit_value(p, (nyr_inst_t){.op = NYIR_LOAD_I64,
                                         .dst = -1,
                                         .a = address,
                                         .effects = NYIR_EFFECT_READ_MEMORY |
                                                    NYIR_EFFECT_MAY_TRAP |
                                                    (field->is_volatile
                                                         ? NYIR_EFFECT_VOLATILE
                                                         : 0),
                                         .flags = NYIR_INST_F_MEM_I32,
                                         .scalar_class = NYIR_SCALAR_I64_RAW})
        : is_u8 ? lower_load_u8_field(p, address, field->is_volatile)
        : is_i16 || is_u16
            ? lower_load_u16_field(p, address, field_type, field->is_volatile)
            : lower_load_field(p, address, is_f64, field->is_volatile);
    if (is_u32 && loaded >= 0)
      loaded = normalize_u32(p, loaded);
    if (loaded < 0)
      return false;
    if (is_i16 || is_u16)
      loaded = normalize_integer(p, field_type, loaded);
    if (is_u8 && field->is_signed_byte)
      loaded = normalize_integer(p, C_CHAR, loaded);
    field_type = is_f32   ? C_F32
                 : is_f64 ? C_F64
                 : is_i32 ? C_I32
                 : is_u32 ? C_U32
                 : is_u8  ? (field->is_signed_byte ? C_CHAR : C_U8)
                 : is_i16 ? C_I16
                 : is_u16 ? C_U16
                 : is_u64 ? C_U64
                          : C_I64;
    if (!record_lvalue(p, loaded, address, field_type, field->is_volatile))
      return false;
    *out = (c_value_t){loaded, field_type, NULL};
    return true;
  }
  *out = value;
  return true;
}

static c_local_t *find_local(c_parser_t *p, ny_ctok_t name) {
  for (size_t i = p->local_count; i > 0; --i)
    if (tok_same(p->locals[i - 1].name, name))
      return &p->locals[i - 1];
  return NULL;
}

static int precedence(c_parser_t *p) {
  if (tok_is(p, p->pos, "||"))
    return 1;
  if (tok_is(p, p->pos, "&&"))
    return 2;
  if (tok_is(p, p->pos, "|"))
    return 3;
  if (tok_is(p, p->pos, "^"))
    return 4;
  if (tok_is(p, p->pos, "&"))
    return 5;
  if (tok_is(p, p->pos, "==") || tok_is(p, p->pos, "!="))
    return 6;
  if (tok_is(p, p->pos, "<") || tok_is(p, p->pos, "<=") ||
      tok_is(p, p->pos, ">") || tok_is(p, p->pos, ">="))
    return 7;
  if (tok_is(p, p->pos, "<<") || tok_is(p, p->pos, ">>"))
    return 8;
  if (tok_is(p, p->pos, "+") || tok_is(p, p->pos, "-"))
    return 9;
  if (tok_is(p, p->pos, "*") || tok_is(p, p->pos, "/") ||
      tok_is(p, p->pos, "%"))
    return 10;
  return 0;
}

static bool is_assignment_operator(const c_parser_t *p, size_t pos) {
  static const char *const operators[] = {
      "=", "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "<<=", ">>=",
  };
  for (size_t i = 0; i < sizeof(operators) / sizeof(operators[0]); ++i)
    if (tok_is(p, pos, operators[i]))
      return true;
  return false;
}

static nyr_op_t compound_assignment_ir(c_type_t type, ny_ctok_t op) {
  if (!c_type_is_integer(type) && !c_type_is_float(type))
    return NYIR_NOP;
  if (ny_ctok_eq(op, "+="))
    return type == C_F32   ? NYIR_ADD_F32
           : type == C_F64 ? NYIR_ADD_F64
                           : NYIR_ADD_I64;
  if (ny_ctok_eq(op, "-="))
    return type == C_F32   ? NYIR_SUB_F32
           : type == C_F64 ? NYIR_SUB_F64
                           : NYIR_SUB_I64;
  if (ny_ctok_eq(op, "*="))
    return type == C_F32   ? NYIR_MUL_F32
           : type == C_F64 ? NYIR_MUL_F64
                           : NYIR_MUL_I64;
  if (ny_ctok_eq(op, "/="))
    return type == C_F32   ? NYIR_DIV_F32
           : type == C_F64 ? NYIR_DIV_F64
                           : NYIR_DIV_I64;
  if (c_type_is_integer(type) && ny_ctok_eq(op, "%="))
    return NYIR_MOD_I64;
  if (c_type_is_integer(type) && ny_ctok_eq(op, "&="))
    return NYIR_AND_I64;
  if (c_type_is_integer(type) && ny_ctok_eq(op, "|="))
    return NYIR_OR_I64;
  if (c_type_is_integer(type) && ny_ctok_eq(op, "^="))
    return NYIR_XOR_I64;
  if (c_type_is_integer(type) && ny_ctok_eq(op, "<<="))
    return NYIR_SHL_I64;
  if (c_type_is_integer(type) && ny_ctok_eq(op, ">>="))
    return NYIR_SAR_I64;
  return NYIR_NOP;
}

static bool c_ir_nonnegative_constant(const c_parser_t *p, int value,
                                      int64_t *constant) {
  if (!p->out || value < 0)
    return false;
  for (size_t i = p->out->len; i > 0; --i) {
    const nyr_inst_t *inst = &p->out->data[i - 1];
    if (inst->dst != value)
      continue;
    if (inst->op != NYIR_CONST_I64 || inst->imm < 0)
      return false;
    *constant = inst->imm;
    return true;
  }
  return false;
}

static bool c_ir_signed_constant(const c_parser_t *p, int value,
                                 int64_t *constant) {
  if (!p->out || value < 0)
    return false;
  for (size_t i = p->out->len; i > 0; --i) {
    const nyr_inst_t *inst = &p->out->data[i - 1];
    if (inst->dst != value)
      continue;
    if (inst->op != NYIR_CONST_I64)
      return false;
    *constant = inst->imm;
    return true;
  }
  return false;
}

static bool c_ir_boolean_constant(const c_parser_t *p, int value,
                                  int64_t *constant) {
  int64_t direct = 0;
  if (c_ir_signed_constant(p, value, &direct)) {
    *constant = direct != 0;
    return true;
  }
  if (!p->out || value < 0)
    return false;
  const nyr_inst_t *compare = NULL;
  for (size_t i = p->out->len; i > 0; --i) {
    const nyr_inst_t *inst = &p->out->data[i - 1];
    if (inst->dst == value) {
      compare = inst;
      break;
    }
  }
  if (!compare || compare->op != NYIR_CMP_I64 ||
      (compare->cmp != NYIR_CMP_EQ && compare->cmp != NYIR_CMP_NE))
    return false;
  int64_t left = 0;
  int64_t right = 0;
  if (!c_ir_signed_constant(p, compare->a, &left) ||
      !c_ir_signed_constant(p, compare->b, &right))
    return false;
  *constant = compare->cmp == NYIR_CMP_EQ ? left == right : left != right;
  return true;
}

static c_function_t *find_builtin_function(ny_ctok_t name, nyc_abi_t abi,
                                           bool no_builtin,
                                           c_function_t *storage) {
  if (no_builtin &&
      (name.len < sizeof("__builtin_") - 1 ||
       memcmp(name.start, "__builtin_", sizeof("__builtin_") - 1) != 0))
    return NULL;
  static const c_param_t memory_fill_params[] = {
      {.type = C_VOID_PTR},
      {.type = C_I32},
      {.type = C_U64},
  };
  static const c_param_t memory_copy_params[] = {
      {.type = C_VOID_PTR},
      {.type = C_VOID_PTR},
      {.type = C_U64},
  };
  static const c_param_t memory_fill_params_i386[] = {
      {.type = C_VOID_PTR}, {.type = C_I32}, {.type = C_U32}};
  static const c_param_t memory_copy_params_i386[] = {
      {.type = C_VOID_PTR}, {.type = C_VOID_PTR}, {.type = C_U32}};
  static const c_param_t one_string_param[] = {{.type = C_CHAR_PTR}};
  static const c_param_t two_string_params[] = {{.type = C_CHAR_PTR},
                                                {.type = C_CHAR_PTR}};
  static const c_param_t string_span_params[] = {{.type = C_CHAR_PTR},
                                                 {.type = C_CHAR_PTR}};
  static const c_param_t string_char_params[] = {{.type = C_CHAR_PTR},
                                                 {.type = C_I32}};
  static const c_param_t bounded_string_params[] = {
      {.type = C_CHAR_PTR}, {.type = C_CHAR_PTR}, {.type = C_U64}};
  static const c_param_t memory_compare_params[] = {
      {.type = C_VOID_PTR}, {.type = C_VOID_PTR}, {.type = C_U64}};
  static const c_param_t memory_find_params[] = {
      {.type = C_VOID_PTR, .pointee_const = true},
      {.type = C_I32},
      {.type = C_U64},
  };
  static const c_param_t allocation_size_param[] = {{.type = C_U64}};
  static const c_param_t allocation_pair_params[] = {{.type = C_U64},
                                                     {.type = C_U64}};
  static const c_param_t reallocation_params[] = {{.type = C_VOID_PTR},
                                                  {.type = C_U64}};
  static const c_param_t deallocation_param[] = {{.type = C_VOID_PTR}};
  static const c_param_t time_pointer_param[] = {{.type = C_I64_PTR}};
  static const c_param_t one_float_param[] = {{.type = C_F64}};
  static const c_param_t two_float_params[] = {{.type = C_F64},
                                               {.type = C_F64}};
  const c_param_t *params = NULL;
  c_type_t result = C_VOID_PTR;
  size_t param_count = 3;
  if (ny_ctok_eq(name, "memset") || ny_ctok_eq(name, "__builtin_memset"))
    params = abi == NYC_ABI_I386 ? memory_fill_params_i386 : memory_fill_params;
  else if (ny_ctok_eq(name, "memcpy") || ny_ctok_eq(name, "memmove") ||
           ny_ctok_eq(name, "__builtin_memcpy") ||
           ny_ctok_eq(name, "__builtin_memmove"))
    params = abi == NYC_ABI_I386 ? memory_copy_params_i386 : memory_copy_params;
  else if (ny_ctok_eq(name, "malloc")) {
    params = allocation_size_param;
    param_count = 1;
  } else if (ny_ctok_eq(name, "calloc")) {
    params = allocation_pair_params;
    param_count = 2;
  } else if (ny_ctok_eq(name, "realloc")) {
    params = reallocation_params;
    param_count = 2;
  } else if (ny_ctok_eq(name, "free")) {
    params = deallocation_param;
    result = C_BAD;
    param_count = 1;
  } else if (ny_ctok_eq(name, "strlen") ||
             ny_ctok_eq(name, "__builtin_strlen")) {
    params = one_string_param;
    result = C_U64;
    param_count = 1;
  } else if (ny_ctok_eq(name, "strspn") || ny_ctok_eq(name, "strcspn")) {
    params = string_span_params;
    result = C_U64;
    param_count = 2;
  } else if (ny_ctok_eq(name, "strchr") || ny_ctok_eq(name, "strrchr")) {
    params = string_char_params;
    result = C_CHAR_PTR;
    param_count = 2;
  } else if (ny_ctok_eq(name, "strcmp") ||
             ny_ctok_eq(name, "__builtin_strcmp") ||
             ny_ctok_eq(name, "strcasecmp") || ny_ctok_eq(name, "stricmp")) {
    params = two_string_params;
    result = C_I32;
    param_count = 2;
  } else if (ny_ctok_eq(name, "strncmp") || ny_ctok_eq(name, "strncasecmp")) {
    params = bounded_string_params;
    result = C_I32;
  } else if (ny_ctok_eq(name, "memcmp") ||
             ny_ctok_eq(name, "__builtin_memcmp")) {
    params = memory_compare_params;
    result = C_I32;
  } else if (ny_ctok_eq(name, "memchr")) {
    params = memory_find_params;
  } else if (ny_ctok_eq(name, "localtime") || ny_ctok_eq(name, "gmtime")) {
    params = time_pointer_param;
    result = C_AGG_PTR;
    param_count = 1;
  } else if (ny_ctok_eq(name, "fabs") || ny_ctok_eq(name, "sqrt") ||
             ny_ctok_eq(name, "floor") || ny_ctok_eq(name, "ceil") ||
             ny_ctok_eq(name, "trunc") || ny_ctok_eq(name, "round") ||
             ny_ctok_eq(name, "sin") || ny_ctok_eq(name, "cos") ||
             ny_ctok_eq(name, "tan") || ny_ctok_eq(name, "atan") ||
             ny_ctok_eq(name, "exp") || ny_ctok_eq(name, "log")) {
    params = one_float_param;
    result = C_F64;
    param_count = 1;
  } else if (ny_ctok_eq(name, "pow") || ny_ctok_eq(name, "hypot")) {
    params = two_float_params;
    result = C_F64;
    param_count = 2;
  }
  if (!params)
    return NULL;
  ny_ctok_t target_name =
      ny_ctok_eq(name, "__builtin_memcpy")
          ? (ny_ctok_t){.start = "memcpy", .len = sizeof("memcpy") - 1}
      : ny_ctok_eq(name, "__builtin_memset")
          ? (ny_ctok_t){.start = "memset", .len = sizeof("memset") - 1}
      : ny_ctok_eq(name, "__builtin_memmove")
          ? (ny_ctok_t){.start = "memmove", .len = sizeof("memmove") - 1}
      : ny_ctok_eq(name, "__builtin_memcmp")
          ? (ny_ctok_t){.start = "memcmp", .len = sizeof("memcmp") - 1}
      : ny_ctok_eq(name, "__builtin_strlen")
          ? (ny_ctok_t){.start = "strlen", .len = sizeof("strlen") - 1}
      : ny_ctok_eq(name, "__builtin_strcmp")
          ? (ny_ctok_t){.start = "strcmp", .len = sizeof("strcmp") - 1}
          : name;
  *storage = (c_function_t){.name = target_name,
                            .result = result,
                            .params = (c_param_t *)params,
                            .param_count = param_count};
  return storage;
}

static c_value_t parse_atomic_builtin(c_parser_t *p, ny_ctok_t name) {
  bool store = ny_ctok_eq(name, "__atomic_store_n") ||
               ny_ctok_eq(name, "__c11_atomic_store");
  c_value_t address = parse_expr(p, 1);
  if (address.value < 0 || !c_type_is_pointer(address.type) ||
      !expect(p, ",")) {
    fail(p, "atomic builtin requires a typed object pointer");
    return (c_value_t){-1, C_BAD, NULL};
  }
  c_type_t type = c_remove_pointer_levels(address.type, 1);
  if (type != C_U8 && type != C_CHAR && type != C_I32 && type != C_U32 &&
      type != C_I16 && type != C_U16 && type != C_I64 && type != C_U64 &&
      type != C_FUNC_PTR && type != C_OPAQUE_PTR && type != C_VOID_PTR) {
    fail(p, "atomic builtin supports 8/16/32/64-bit integers and pointers");
    return (c_value_t){-1, C_BAD, NULL};
  }

  c_value_t value = {-1, C_BAD, NULL};
  bool null_pointer_constant = false;
  if (store) {
    value = parse_expr(p, 1);
    null_pointer_constant = p->last_value_null_pointer_constant;
    if (value.value < 0 || !expect(p, ",")) {
      fail(p, "__atomic_store_n requires a value and memory order");
      return (c_value_t){-1, C_BAD, NULL};
    }
    if ((c_type_is_pointer(type) && type != value.type &&
         !c_pointer_compatible(type, value.type) &&
         !(c_type_is_integer(value.type) && null_pointer_constant)) ||
        (c_type_is_integer(type) && !c_type_is_integer(value.type))) {
      fail(p, "__atomic_store_n value type is incompatible with its object");
      return (c_value_t){-1, C_BAD, NULL};
    }
  }

  c_value_t order = parse_expr(p, 1);
  int64_t order_value;
  if (order.value < 0 || !expect(p, ")") ||
      !c_ir_nonnegative_constant(p, order.value, &order_value) ||
      order_value != 0) {
    fail(p, "NYC atomic builtins currently require __ATOMIC_RELAXED");
    return (c_value_t){-1, C_BAD, NULL};
  }

  if (store) {
    bool stored =
        type == C_U8 || type == C_CHAR
            ? lower_store_u8_field(p, address.value, value.value, true)
        : type == C_I16 || type == C_U16
            ? emit(p,
                   (nyr_inst_t){
                       .op = NYIR_STORE_I64,
                       .dst = -1,
                       .a = address.value,
                       .c = value.value,
                       .effects = NYIR_EFFECT_WRITE_MEMORY |
                                  NYIR_EFFECT_MAY_TRAP | NYIR_EFFECT_VOLATILE,
                       .flags = NYIR_INST_F_MEM_U16 | NYIR_INST_F_MEM_VOLATILE,
                       .scalar_class = NYIR_SCALAR_I64_RAW},
                   NULL)
        : type == C_I32 || type == C_U32
            ? lower_store_i32_field(p, address.value, value.value, true)
        : c_type_is_pointer(type)
            ? lower_store_pointer_field(p, address.value, value.value, true)
        : type == C_I64 || type == C_U64
            ? lower_store_field(p, address.value, value.value, false, true)
            : lower_store_field(p, address.value, value.value, false, true);
    if (!stored)
      return (c_value_t){-1, C_BAD, NULL};
    return (c_value_t){-1, C_BAD, NULL};
  }

  int loaded =
      type == C_U8 || type == C_CHAR
          ? lower_load_u8_field(p, address.value, true)
      : type == C_I16 || type == C_U16 ? lower_load_atomic_u16(p, address.value)
      : type == C_I32 ? lower_load_i32_field(p, address.value, true)
      : type == C_U32
          ? normalize_u32(p, lower_load_i32_field(p, address.value, true))
          : lower_load_field(p, address.value, false, true);
  if (loaded >= 0 && c_type_is_integer(type))
    loaded = normalize_integer(p, type, loaded);
  return loaded < 0 ? (c_value_t){-1, C_BAD, NULL}
                    : (c_value_t){loaded, type, NULL};
}

static c_value_t parse_byte_swap_builtin(c_parser_t *p, ny_ctok_t name) {
  unsigned bits = ny_ctok_eq(name, "__builtin_bswap16")   ? 16
                  : ny_ctok_eq(name, "__builtin_bswap32") ? 32
                                                          : 64;
  c_value_t value = parse_expr(p, 1);
  if (value.value < 0 || !c_type_is_integer(value.type) || !expect(p, ")")) {
    fail(p, "byte-swap builtin requires one integer argument");
    return (c_value_t){-1, C_BAD, NULL};
  }
  if (bits == 64) {
    int swapped =
        emit_value(p, (nyr_inst_t){.op = NYIR_BSWAP_I64,
                                   .dst = -1,
                                   .a = value.value,
                                   .scalar_class = NYIR_SCALAR_I64_RAW});
    return swapped < 0 ? (c_value_t){-1, C_BAD, NULL}
                       : (c_value_t){swapped, C_U64, NULL};
  }
  if (bits == 32) {
    int input = normalize_u32(p, value.value);
    int swapped =
        input < 0
            ? -1
            : emit_value(p, (nyr_inst_t){.op = NYIR_BSWAP32_I64,
                                         .dst = -1,
                                         .a = input,
                                         .scalar_class = NYIR_SCALAR_I64_RAW});
    return swapped < 0 ? (c_value_t){-1, C_BAD, NULL}
                       : (c_value_t){swapped, C_U32, NULL};
  }

  int input = normalize_u16(p, value.value);
  int low = lower_bitfield_binary(p, NYIR_AND_I64, input, 0xff);
  low = lower_bitfield_binary(p, NYIR_SHL_I64, low, 8);
  int high = lower_bitfield_binary(p, NYIR_SAR_I64, input, 8);
  high = lower_bitfield_binary(p, NYIR_AND_I64, high, 0xff);
  int swapped =
      low < 0 || high < 0 ? -1 : emit_i64_binary(p, NYIR_OR_I64, low, high);
  swapped = normalize_u16(p, swapped);
  return swapped < 0 ? (c_value_t){-1, C_BAD, NULL}
                     : (c_value_t){swapped, C_U16, NULL};
}

static c_value_t lower_clzll_value(c_parser_t *p, int input) {
  static const uint64_t masks[] = {
      UINT64_C(0xffffffff00000000), UINT64_C(0xffff000000000000),
      UINT64_C(0xff00000000000000), UINT64_C(0xf000000000000000),
      UINT64_C(0xc000000000000000), UINT64_C(0x8000000000000000)};
  static const unsigned shifts[] = {32, 16, 8, 4, 2, 1};
  int remaining = input;
  int count = emit_i64_constant(p, 0);
  for (size_t i = 0; i < sizeof(shifts) / sizeof(shifts[0]); ++i) {
    int high = lower_bitfield_binary(p, NYIR_AND_I64, remaining, masks[i]);
    int zero = emit_i64_constant(p, 0);
    int is_zero = high < 0 || zero < 0
                      ? -1
                      : emit_i64_compare(p, high, zero, NYIR_CMP_EQ);
    int shifted = lower_bitfield_binary(p, NYIR_SHL_I64, remaining, shifts[i]);
    int increment = emit_i64_constant(p, shifts[i]);
    if (is_zero < 0 || shifted < 0 || increment < 0 || count < 0)
      return (c_value_t){-1, C_BAD, NULL};
    remaining = emit_i64_select(p, is_zero, shifted, remaining);
    increment = emit_i64_select(p, is_zero, increment, zero);
    if (remaining < 0 || increment < 0)
      return (c_value_t){-1, C_BAD, NULL};
    count = emit_i64_binary(p, NYIR_ADD_I64, count, increment);
  }
  return count < 0 ? (c_value_t){-1, C_BAD, NULL}
                   : (c_value_t){count, C_I32, NULL};
}

static c_value_t parse_clzll_builtin(c_parser_t *p) {
  c_value_t value = parse_expr(p, 1);
  if (value.value < 0 || !c_type_is_integer(value.type) || !expect(p, ")")) {
    fail(p, "__builtin_clzll requires one integer argument");
    return (c_value_t){-1, C_BAD, NULL};
  }
  return lower_clzll_value(p, value.value);
}

static c_value_t parse_clz_builtin(c_parser_t *p) {
  c_value_t value = parse_expr(p, 1);
  if (value.value < 0 || !c_type_is_integer(value.type) || !expect(p, ")")) {
    fail(p, "__builtin_clz requires one integer argument");
    return (c_value_t){-1, C_BAD, NULL};
  }
  int narrowed = normalize_u32(p, value.value);
  c_value_t count = lower_clzll_value(p, narrowed);
  int width = emit_i64_constant(p, 32);
  int result = count.value < 0 || width < 0
                   ? -1
                   : emit_i64_binary(p, NYIR_SUB_I64, count.value, width);
  return result < 0 ? (c_value_t){-1, C_BAD, NULL}
                    : (c_value_t){result, C_I32, NULL};
}

static int emit_i64_compare(c_parser_t *p, int left, int right,
                            nyr_cmp_t comparison);

static c_value_t lower_ctz_value(c_parser_t *p, int input) {
  static const uint64_t masks[] = {UINT64_C(0xffffffff), UINT64_C(0xffff),
                                   UINT64_C(0xff),       UINT64_C(0xf),
                                   UINT64_C(0x3),        UINT64_C(0x1)};
  static const uint64_t right_masks[] = {
      UINT64_C(0x00000000ffffffff), UINT64_C(0x0000ffffffffffff),
      UINT64_C(0x00ffffffffffffff), UINT64_C(0x0fffffffffffffff),
      UINT64_C(0x3fffffffffffffff), UINT64_C(0x7fffffffffffffff)};
  static const unsigned shifts[] = {32, 16, 8, 4, 2, 1};
  int remaining = input;
  int zero = emit_i64_constant(p, 0);
  int count = zero;
  for (size_t i = 0; i < sizeof(shifts) / sizeof(shifts[0]); ++i) {
    int low = lower_bitfield_binary(p, NYIR_AND_I64, remaining, masks[i]);
    int is_zero =
        low < 0 || zero < 0 ? -1 : emit_i64_compare(p, low, zero, NYIR_CMP_EQ);
    int nonnegative =
        lower_bitfield_binary(p, NYIR_AND_I64, remaining, right_masks[i]);
    int shifted =
        nonnegative < 0
            ? -1
            : lower_bitfield_binary(p, NYIR_SAR_I64, nonnegative, shifts[i]);
    int increment = emit_i64_constant(p, shifts[i]);
    if (is_zero < 0 || shifted < 0 || increment < 0 || count < 0)
      return (c_value_t){-1, C_BAD, NULL};
    remaining = emit_i64_select(p, is_zero, shifted, remaining);
    increment = emit_i64_select(p, is_zero, increment, zero);
    if (remaining < 0 || increment < 0)
      return (c_value_t){-1, C_BAD, NULL};
    count = emit_i64_binary(p, NYIR_ADD_I64, count, increment);
  }
  return count < 0 ? (c_value_t){-1, C_BAD, NULL}
                   : (c_value_t){count, C_I32, NULL};
}

static c_value_t parse_ctzl_builtin(c_parser_t *p) {
  c_value_t value = parse_expr(p, 1);
  if (value.value < 0 || !c_type_is_integer(value.type) || !expect(p, ")")) {
    fail(p, "count-trailing-zero builtin requires one integer argument");
    return (c_value_t){-1, C_BAD, NULL};
  }
  return lower_ctz_value(p, value.value);
}

static c_value_t parse_ffs_builtin(c_parser_t *p) {
  c_value_t value = parse_expr(p, 1);
  if (value.value < 0 || !c_type_is_integer(value.type) || !expect(p, ")")) {
    fail(p, "__builtin_ffs requires one integer argument");
    return (c_value_t){-1, C_BAD, NULL};
  }
  c_value_t trailing = lower_ctz_value(p, value.value);
  int one = emit_i64_constant(p, 1);
  int zero = emit_i64_constant(p, 0);
  int is_zero = emit_i64_compare(p, value.value, zero, NYIR_CMP_EQ);
  int count = trailing.value < 0 || one < 0
                  ? -1
                  : emit_i64_binary(p, NYIR_ADD_I64, trailing.value, one);
  int result =
      is_zero < 0 || count < 0 ? -1 : emit_i64_select(p, is_zero, zero, count);
  return result < 0 ? (c_value_t){-1, C_BAD, NULL}
                    : (c_value_t){result, C_I32, NULL};
}

static int emit_i64_compare(c_parser_t *p, int left, int right,
                            nyr_cmp_t comparison) {
  return emit_value(p, (nyr_inst_t){.op = NYIR_CMP_I64,
                                    .dst = -1,
                                    .a = left,
                                    .b = right,
                                    .cmp = comparison,
                                    .scalar_class = NYIR_SCALAR_I64_RAW});
}

static int emit_i64_binary(c_parser_t *p, nyr_op_t op, int left, int right) {
  return emit_value(p, (nyr_inst_t){.op = op,
                                    .dst = -1,
                                    .a = left,
                                    .b = right,
                                    .scalar_class = NYIR_SCALAR_I64_RAW});
}

static int emit_i64_select(c_parser_t *p, int condition, int when_true,
                           int when_false) {
  return emit_value(p, (nyr_inst_t){.op = NYIR_SELECT_I64,
                                    .dst = -1,
                                    .a = condition,
                                    .b = when_true,
                                    .c = when_false,
                                    .scalar_class = NYIR_SCALAR_I64_RAW});
}

static int emit_u64_compare(c_parser_t *p, int left, int right,
                            nyr_cmp_t comparison) {
  int sign = emit_i64_constant(p, INT64_MIN);
  int biased_left = emit_i64_binary(p, NYIR_XOR_I64, left, sign);
  int biased_right = emit_i64_binary(p, NYIR_XOR_I64, right, sign);
  return sign < 0 || biased_left < 0 || biased_right < 0
             ? -1
             : emit_i64_compare(p, biased_left, biased_right, comparison);
}

static int emit_u64_multiply_high(c_parser_t *p, int left, int right) {
  const uint64_t low_mask = UINT64_C(0xffffffff);
  int left_low = lower_bitfield_binary(p, NYIR_AND_I64, left, low_mask);
  int left_high = lower_bitfield_binary(
      p, NYIR_AND_I64, lower_bitfield_binary(p, NYIR_SAR_I64, left, 32),
      low_mask);
  int right_low = lower_bitfield_binary(p, NYIR_AND_I64, right, low_mask);
  int right_high = lower_bitfield_binary(
      p, NYIR_AND_I64, lower_bitfield_binary(p, NYIR_SAR_I64, right, 32),
      low_mask);
  int low_product = emit_i64_binary(p, NYIR_MUL_I64, left_low, right_low);
  int low_product_high = lower_bitfield_binary(
      p, NYIR_AND_I64, lower_bitfield_binary(p, NYIR_SAR_I64, low_product, 32),
      low_mask);
  int cross_left = emit_i64_binary(p, NYIR_MUL_I64, left_high, right_low);
  int middle = emit_i64_binary(p, NYIR_ADD_I64, cross_left, low_product_high);
  int middle_low = lower_bitfield_binary(p, NYIR_AND_I64, middle, low_mask);
  int middle_high = lower_bitfield_binary(
      p, NYIR_AND_I64, lower_bitfield_binary(p, NYIR_SAR_I64, middle, 32),
      low_mask);
  int cross_right = emit_i64_binary(p, NYIR_MUL_I64, left_low, right_high);
  int high_middle = emit_i64_binary(p, NYIR_ADD_I64, cross_right, middle_low);
  int high_middle_high = lower_bitfield_binary(
      p, NYIR_AND_I64, lower_bitfield_binary(p, NYIR_SAR_I64, high_middle, 32),
      low_mask);
  int high_product = emit_i64_binary(p, NYIR_MUL_I64, left_high, right_high);
  int high_with_carry =
      emit_i64_binary(p, NYIR_ADD_I64, high_product, middle_high);
  return emit_i64_binary(p, NYIR_ADD_I64, high_with_carry, high_middle_high);
}

static c_value_t parse_overflow_builtin(c_parser_t *p, ny_ctok_t name) {
  bool add = ny_ctok_eq(name, "__builtin_add_overflow");
  bool subtract = ny_ctok_eq(name, "__builtin_sub_overflow");
  c_value_t left = parse_expr(p, 1);
  if (left.value < 0 || !c_type_is_integer(left.type) || !expect(p, ",")) {
    fail(p, "overflow builtin requires integer operands");
    return (c_value_t){-1, C_BAD, NULL};
  }
  c_value_t right = parse_expr(p, 1);
  if (right.value < 0 || !c_type_is_integer(right.type) || !expect(p, ",")) {
    fail(p, "overflow builtin requires integer operands");
    return (c_value_t){-1, C_BAD, NULL};
  }
  c_value_t output = parse_expr(p, 1);
  bool unsigned_u64 = output.type == C_U64_PTR;
  bool signed_i64 = output.type == C_I64_PTR;
  bool signed_i32 = output.type == C_I32_PTR;
  bool unsigned_u32 = output.type == C_U32_PTR;
  bool signed_i16 = output.type == C_I16_PTR;
  bool unsigned_u16 = output.type == C_U16_PTR;
  bool signed_i8 = output.type == C_I8_PTR;
  bool unsigned_u8 = output.type == C_CHAR_PTR;
  if (output.value < 0 ||
      (!unsigned_u64 && !signed_i64 && !signed_i32 && !unsigned_u32 &&
       !signed_i16 && !unsigned_u16 && !signed_i8 && !unsigned_u8) ||
      !expect(p, ")")) {
    fail(p, "overflow builtin requires an integer output pointer");
    return (c_value_t){-1, C_BAD, NULL};
  }

  nyr_op_t operation = add        ? NYIR_ADD_I64
                       : subtract ? NYIR_SUB_I64
                                  : NYIR_MUL_I64;
  int result = emit_i64_binary(p, operation, left.value, right.value);
  int zero = emit_i64_constant(p, 0);
  int overflow = -1;
  if (signed_i8 || unsigned_u8 || signed_i16 || unsigned_u16 || signed_i32 ||
      unsigned_u32) {
    c_type_t output_integer_type = signed_i8      ? C_CHAR
                                   : unsigned_u8  ? C_U8
                                   : signed_i16   ? C_I16
                                   : unsigned_u16 ? C_U16
                                   : signed_i32   ? C_I32
                                                  : C_U32;
    int narrowed = normalize_integer(p, output_integer_type, result);
    overflow =
        narrowed < 0 ? -1 : emit_i64_compare(p, result, narrowed, NYIR_CMP_NE);
  } else if (unsigned_u64 && (add || subtract)) {
    overflow = add ? emit_u64_compare(p, result, left.value, NYIR_CMP_LT)
                   : emit_u64_compare(p, left.value, right.value, NYIR_CMP_LT);
  } else if (unsigned_u64) {
    int high = emit_u64_multiply_high(p, left.value, right.value);
    overflow = emit_i64_compare(p, high, zero, NYIR_CMP_NE);
  } else if (add || subtract) {
    int left_sign_bits =
        lower_bitfield_binary(p, NYIR_AND_I64, left.value, (uint64_t)INT64_MIN);
    int right_sign_bits = lower_bitfield_binary(p, NYIR_AND_I64, right.value,
                                                (uint64_t)INT64_MIN);
    int result_sign_bits =
        lower_bitfield_binary(p, NYIR_AND_I64, result, (uint64_t)INT64_MIN);
    int left_sign = emit_i64_compare(p, left_sign_bits, zero, NYIR_CMP_NE);
    int right_sign = emit_i64_compare(p, right_sign_bits, zero, NYIR_CMP_NE);
    int result_sign = emit_i64_compare(p, result_sign_bits, zero, NYIR_CMP_NE);
    int sign_relation = emit_i64_compare(p, left_sign, right_sign,
                                         add ? NYIR_CMP_EQ : NYIR_CMP_NE);
    int result_relation =
        emit_i64_compare(p, left_sign, result_sign, NYIR_CMP_NE);
    overflow = emit_i64_binary(p, NYIR_AND_I64, sign_relation, result_relation);
  } else {
    int minus_one = emit_i64_constant(p, -1);
    int smallest = emit_i64_constant(p, INT64_MIN);
    int left_is_smallest =
        emit_i64_compare(p, left.value, smallest, NYIR_CMP_EQ);
    int right_is_minus_one =
        emit_i64_compare(p, right.value, minus_one, NYIR_CMP_EQ);
    int special =
        emit_i64_binary(p, NYIR_AND_I64, left_is_smallest, right_is_minus_one);
    int right_is_zero = emit_i64_compare(p, right.value, zero, NYIR_CMP_EQ);
    int unsafe = emit_i64_binary(p, NYIR_OR_I64, special, right_is_zero);
    int unsafe_condition = emit_i64_compare(p, unsafe, zero, NYIR_CMP_NE);
    int one = emit_i64_constant(p, 1);
    int divisor = emit_i64_select(p, unsafe_condition, one, right.value);
    int quotient = emit_i64_binary(p, NYIR_DIV_I64, result, divisor);
    int mismatch = emit_i64_compare(p, quotient, left.value, NYIR_CMP_NE);
    int right_nonzero = emit_i64_compare(p, right.value, zero, NYIR_CMP_NE);
    int ordinary = emit_i64_binary(p, NYIR_AND_I64, mismatch, right_nonzero);
    overflow = emit_i64_binary(p, NYIR_OR_I64, special, ordinary);
  }
  if (result < 0 || overflow < 0 ||
      !emit(p,
            (nyr_inst_t){
                .op = NYIR_STORE_I64,
                .dst = -1,
                .a = output.value,
                .c = result,
                .effects = NYIR_EFFECT_WRITE_MEMORY | NYIR_EFFECT_MAY_TRAP,
                .flags = signed_i8 || unsigned_u8     ? NYIR_INST_F_MEM_BYTE
                         : signed_i16 || unsigned_u16 ? NYIR_INST_F_MEM_U16
                         : signed_i32 || unsigned_u32 ? NYIR_INST_F_MEM_I32
                                                      : 0,
                .scalar_class = NYIR_SCALAR_I64_RAW},
            NULL))
    return (c_value_t){-1, C_BAD, NULL};
  return (c_value_t){overflow, C_I32, NULL};
}

static bool transparent_union_accepts_pointer(const c_agg_type_t *aggregate,
                                              c_type_t value_type,
                                              bool is_null) {
  if (!aggregate || !aggregate->is_transparent_union || !aggregate->is_union ||
      aggregate->size != sizeof(uint64_t))
    return false;
  for (size_t i = 0; i < aggregate->field_count; ++i) {
    const c_agg_field_t *field = &aggregate->fields[i];
    if (field->kind != C_AGG_FIELD_PTR || field->size != sizeof(uint64_t))
      continue;
    c_type_t member_type =
        c_add_pointer_levels((c_type_t)field->pointer_element_type, 1);
    if (!c_type_is_pointer(member_type))
      continue;
    if (is_null || value_type == member_type ||
        c_pointer_compatible(value_type, member_type))
      return true;
  }
  return false;
}

static c_value_t lower_aggregate_call_result(c_parser_t *p, int result,
                                             const c_agg_type_t *aggregate,
                                             uint32_t packed) {
  /*
   * Capture secondary return registers before stores/address arithmetic:
   * CAPTURE_RET instructions must remain adjacent to the call.
   */
  if (result < 0)
    return (c_value_t){-1, C_BAD, NULL};
  int returned[4] = {result, -1, -1, -1};
  unsigned class0 = NYIR_ARG_AGG_CLASS(packed, 0);
  unsigned class1 = NYIR_ARG_AGG_CLASS(packed, 1);
  if (class0 == NYIR_ARG_CLASS_HFA_F64) {
    unsigned count = NYIR_ARG_AGG_SIZE(packed) / 8u;
    if (count == 0 || count > 4)
      return (c_value_t){-1, C_BAD, NULL};
    static const int selectors[4] = {2, 3, 8, 9};
    for (unsigned piece = 0; piece < count; ++piece) {
      returned[piece] = emit_value(p, (nyr_inst_t){.op = NYIR_CAPTURE_RET,
                                                   .dst = -1,
                                                   .a = -1,
                                                   .b = -1,
                                                   .imm = selectors[piece]});
      if (returned[piece] < 0)
        return (c_value_t){-1, C_BAD, NULL};
    }
  } else {
    if (class0 == NYIR_ARG_CLASS_SSE && class1 == NYIR_ARG_CLASS_INTEGER) {
      /* RAX holds the first INTEGER eightbyte. Capture it before reading
       * XMM0, whose bitwise capture may itself use RAX as scratch. */
      returned[1] = emit_value(
          p,
          (nyr_inst_t){
              .op = NYIR_CAPTURE_RET, .dst = -1, .a = -1, .b = -1, .imm = 1});
      if (returned[1] < 0)
        return (c_value_t){-1, C_BAD, NULL};
    }
    if (class0 == NYIR_ARG_CLASS_SSE) {
      returned[0] = emit_value(
          p,
          (nyr_inst_t){
              .op = NYIR_CAPTURE_RET, .dst = -1, .a = -1, .b = -1, .imm = 2});
      if (returned[0] < 0)
        return (c_value_t){-1, C_BAD, NULL};
    }
    if (returned[1] < 0 &&
        (class1 == NYIR_ARG_CLASS_INTEGER || class1 == NYIR_ARG_CLASS_SSE)) {
      /*
       * SSE eightbytes fill the return-register sequence in order: an
       * INTEGER/SSE pair returns its floating half in xmm0, while SSE/SSE
       * uses xmm0 then xmm1. Capturing xmm1 unconditionally read a stale
       * register for the floating half of mixed-class results.
       */
      unsigned selector = class1 == NYIR_ARG_CLASS_INTEGER
                              ? 0u
                              : (class0 == NYIR_ARG_CLASS_SSE ? 3u : 2u);
      returned[1] = emit_value(p, (nyr_inst_t){.op = NYIR_CAPTURE_RET,
                                               .dst = -1,
                                               .a = -1,
                                               .b = -1,
                                               .imm = (int64_t)selector});
      if (returned[1] < 0)
        return (c_value_t){-1, C_BAD, NULL};
    }
  }
  size_t offset = 0;
  if (!agg_area_alloc(p, aggregate->size, &offset))
    return (c_value_t){-1, C_BAD, NULL};
  int address = lower_add_const(p, p->agg_base, offset);
  if (address < 0)
    return (c_value_t){-1, C_BAD, NULL};
  unsigned pieces = (NYIR_ARG_AGG_SIZE(packed) + 7u) / 8u;
  if (pieces == 0 || pieces > 4)
    return (c_value_t){-1, C_BAD, NULL};
  for (unsigned piece = 0; piece < pieces; ++piece) {
    int at = piece ? lower_add_const(p, address, (size_t)piece * 8u) : address;
    if (at < 0 || returned[piece] < 0 ||
        !lower_store_field(p, at, returned[piece], false, false))
      return (c_value_t){-1, C_BAD, NULL};
  }
  return (c_value_t){address, C_AGG, aggregate};
}

static bool c_va_start_add_stack_argument(size_t size, size_t alignment,
                                          size_t *stack_bytes) {
  if (size == 0)
    return true;
  if (alignment < 8)
    alignment = 8;
  if (alignment > 16 || (alignment & (alignment - 1)) != 0 ||
      size > SIZE_MAX - 7 || *stack_bytes > SIZE_MAX - (alignment - 1))
    return false;
  size_t aligned = (*stack_bytes + alignment - 1) & ~(alignment - 1);
  size_t rounded_size = (size + 7) & ~(size_t)7;
  if (aligned > UINT32_MAX || rounded_size > UINT32_MAX - aligned)
    return false;
  *stack_bytes = aligned + rounded_size;
  return true;
}

static bool c_va_start_account_parameter(c_parser_t *p,
                                         const c_param_t *parameter,
                                         unsigned *gp, unsigned *fp,
                                         size_t *stack_bytes) {
  if (p->abi == NYC_ABI_I386) {
    size_t size = parameter->type == C_AGG
                      ? parameter->agg ? parameter->agg->size : 0
                      : c_type_size(p->abi, parameter->type);
    if ((!size && parameter->type != C_AGG) || size > SIZE_MAX - 3)
      return false;
    size = (size + 3u) & ~(size_t)3u;
    if (size > UINT32_MAX || *stack_bytes > UINT32_MAX - size)
      return false;
    *stack_bytes += size;
    return true;
  }
  if (parameter->type == C_AGG) {
    uint32_t packed = 0;
    const c_agg_type_t *aggregate = parameter->agg;
    if (p->abi != NYC_ABI_SYSV_X86_64 || !aggregate ||
        !nyc_classify_aggregate(aggregate, p->abi, 0, &packed))
      return false;
    if (aggregate->size == 0)
      return true;
    unsigned needed_gp = 0;
    unsigned needed_fp = 0;
    bool stack_argument =
        NYIR_ARG_AGG_CLASS(packed, 0) == NYIR_ARG_CLASS_MEMORY;
    for (unsigned word = 0; word < 2 && !stack_argument; ++word) {
      unsigned cls = NYIR_ARG_AGG_CLASS(packed, word);
      if (cls == NYIR_ARG_CLASS_NONE)
        continue;
      if (cls == NYIR_ARG_CLASS_INTEGER)
        ++needed_gp;
      else if (cls == NYIR_ARG_CLASS_SSE)
        ++needed_fp;
      else
        return false;
    }
    if (!stack_argument && (needed_gp > 6 - *gp || needed_fp > 8 - *fp))
      stack_argument = true;
    if (!stack_argument) {
      *gp += needed_gp;
      *fp += needed_fp;
      return true;
    }
    return c_va_start_add_stack_argument(aggregate->size, aggregate->align,
                                         stack_bytes);
  }
  if (parameter->type == C_F32 || parameter->type == C_F64) {
    if (*fp < 8) {
      ++*fp;
      return true;
    }
  } else if (*gp < 6) {
    ++*gp;
    return true;
  }
  return c_va_start_add_stack_argument(8, 8, stack_bytes);
}

static bool c_store_i386_va_list(c_parser_t *p, const c_lvalue_t *list,
                                 int value) {
  if (!list || list->bitfield)
    return false;
  if (list->is_local) {
    c_local_t *local = find_local_slot(p, list->local_slot);
    return local && !local->is_const && lower_local_store(p, local, value);
  }
  return list->address >= 0 &&
         emit(p,
              (nyr_inst_t){.op = NYIR_STORE_I64,
                           .dst = -1,
                           .a = list->address,
                           .c = value,
                           .flags = NYIR_INST_F_MEM_I32,
                           .effects =
                               NYIR_EFFECT_WRITE_MEMORY | NYIR_EFFECT_MAY_TRAP |
                               (list->is_volatile ? NYIR_EFFECT_VOLATILE : 0)},
              NULL);
}

static int lower_sysv_va_align_16(c_parser_t *p, int address) {
  int biased = lower_add_const(p, address, 8);
  int mask = emit_i64_constant(p, -16);
  return biased < 0 || mask < 0
             ? -1
             : emit_i64_binary(p, NYIR_AND_I64, biased, mask);
}

static int lower_sysv_va_overflow(c_parser_t *p, int list,
                                  const c_agg_type_t *type) {
  int field = lower_add_const(p, list, 8);
  int address = field < 0 ? -1 : lower_load_field(p, field, false, false);
  if (address >= 0 && type->align == 16)
    address = lower_sysv_va_align_16(p, address);
  int next =
      address < 0 ? -1 : lower_add_const(p, address, align_up(type->size, 8));
  return next >= 0 && lower_store_field(p, field, next, false, false) ? address
                                                                      : -1;
}

static int lower_sysv_va_aggregate(c_parser_t *p, int list,
                                   const c_agg_type_t *type) {
  uint32_t packed;
  if (p->abi != NYC_ABI_SYSV_X86_64 || !type || !type->size ||
      type->align > 16 || type->size > NYIR_ARG_AGG_SIZE_MASK ||
      !nyc_classify_argument(type, p->abi, 0, &packed)) {
    fail(p, "va_arg aggregate has an unsupported ABI layout");
    return -1;
  }
  unsigned cls[2] = {NYIR_ARG_AGG_CLASS(packed, 0),
                     NYIR_ARG_AGG_CLASS(packed, 1)};
  if (cls[0] == NYIR_ARG_CLASS_MEMORY)
    return lower_sysv_va_overflow(p, list, type);

  const unsigned step[2] = {8, 16};
  const unsigned end[2] = {48, 176};
  unsigned pieces = type->size > 8 ? 2 : 1;
  unsigned count[2] = {0, 0};
  for (unsigned i = 0; i < pieces; ++i)
    ++count[cls[i] == NYIR_ARG_CLASS_SSE];
  if (count[1] && p->disable_sse) {
    fail(p, "va_arg aggregate requires SSE argument registers");
    return -1;
  }
  if (p->next_label > INT_MAX - 2 || p->next_local == SIZE_MAX)
    return -1;
  int64_t overflow = p->next_label++;
  int64_t done = p->next_label++;
  size_t slot = p->next_local++;
  int field[2], offset[2], next[2];
  for (unsigned bank = 0; bank < 2; ++bank) {
    if (!count[bank])
      continue;
    field[bank] = lower_add_const(p, list, 4 * bank);
    if (field[bank] < 0)
      return -1;
    offset[bank] =
        emit_value(p, (nyr_inst_t){.op = NYIR_LOAD_I64,
                                   .dst = -1,
                                   .a = field[bank],
                                   .flags = NYIR_INST_F_MEM_I32,
                                   .scalar_class = NYIR_SCALAR_I64_RAW,
                                   .effects = NYIR_EFFECT_READ_MEMORY |
                                              NYIR_EFFECT_MAY_TRAP});
    unsigned needed = step[bank] * count[bank];
    int limit = emit_i64_constant(p, end[bank] - needed);
    if (offset[bank] < 0 || limit < 0)
      return -1;
    int exhausted = emit_i64_compare(p, offset[bank], limit, NYIR_CMP_GT);
    if (exhausted < 0 || !emit_branch_if(p, exhausted, overflow))
      return -1;
    next[bank] = lower_add_const(p, offset[bank], needed);
    if (next[bank] < 0)
      return -1;
  }

  // A register aggregate is all-or-nothing: do not advance either bank
  // until both have room. SSE slots are 16 bytes apart in the save area.
  int save_field = lower_add_const(p, list, 16);
  int save =
      save_field < 0 ? -1 : lower_load_field(p, save_field, false, false);
  if (save < 0)
    return -1;
  int address[2];
  for (unsigned bank = 0; bank < 2; ++bank) {
    if (!count[bank])
      continue;
    address[bank] = emit_i64_binary(p, NYIR_ADD_I64, save, offset[bank]);
    if (address[bank] < 0 ||
        !lower_store_i32_field(p, field[bank], next[bank], false))
      return -1;
  }
  unsigned first = cls[0] == NYIR_ARG_CLASS_SSE;
  int result = address[first];
  bool realign = type->align == 16;
  if (realign || (pieces == 2 && count[1])) {
    result = emit_value(p, (nyr_inst_t){.op = NYIR_ALLOCA,
                                        .dst = -1,
                                        .imm = 8 * (pieces + realign),
                                        .scalar_class = NYIR_SCALAR_I64_RAW});
    if (result >= 0 && realign)
      result = lower_sysv_va_align_16(p, result);
    if (result < 0)
      return -1;
    for (unsigned i = 0; i < pieces; ++i) {
      unsigned bank = cls[i] == NYIR_ARG_CLASS_SSE;
      int src = lower_add_const(p, address[bank],
                                i && bank == first ? step[bank] : 0);
      int dst = lower_add_const(p, result, 8 * i);
      int bits = src < 0 ? -1 : lower_load_field(p, src, false, false);
      if (dst < 0 || bits < 0 || !lower_store_field(p, dst, bits, false, false))
        return -1;
    }
  }
  nyr_inst_t merge = {.op = NYIR_STORE_LOCAL,
                      .dst = -1,
                      .a = result,
                      .imm = (int64_t)slot,
                      .scalar_class = NYIR_SCALAR_I64_RAW};
  if (!emit(p, merge, NULL) || !emit_branch(p, done) ||
      !emit_label(p, overflow))
    return -1;
  merge.a = lower_sysv_va_overflow(p, list, type);
  if (merge.a < 0 || !emit(p, merge, NULL) || !emit_label(p, done))
    return -1;
  return emit_value(p, (nyr_inst_t){.op = NYIR_LOAD_LOCAL,
                                    .dst = -1,
                                    .imm = (int64_t)slot,
                                    .scalar_class = NYIR_SCALAR_I64_RAW});
}

static c_value_t parse_call(c_parser_t *p, ny_ctok_t name) {
  if (ny_ctok_eq(name, "__builtin_strcmp") && p->pos < p->token_count &&
      p->tokens[p->pos].kind == NY_CTOK_STRING) {
    c_parser_t probe = *p;
    unsigned char *left = NULL;
    unsigned char *right = NULL;
    size_t left_len = 0;
    size_t right_len = 0;
    if (decode_adjacent_string_tokens(&probe, &left, &left_len) &&
        accept(&probe, ",") && probe.pos < probe.token_count &&
        probe.tokens[probe.pos].kind == NY_CTOK_STRING &&
        decode_adjacent_string_tokens(&probe, &right, &right_len) &&
        accept(&probe, ")")) {
      size_t i = 0;
      while (i < left_len && i < right_len && left[i] == right[i] && left[i])
        ++i;
      int64_t comparison = i < left_len && i < right_len
                               ? (int64_t)left[i] - (int64_t)right[i]
                           : i == left_len ? 0
                                           : (int64_t)left[i];
      free(left);
      free(right);
      p->pos = probe.pos;
      int result = emit_i64_constant(p, comparison);
      return result < 0 ? (c_value_t){-1, C_BAD, NULL}
                        : (c_value_t){result, C_I32, NULL};
    }
    free(left);
    free(right);
  }
  if (ny_ctok_eq(name, "__builtin_alloca")) {
    c_value_t size = parse_expr(p, 1);
    if (size.value < 0 || !c_type_is_integer(size.type) || !expect(p, ")")) {
      fail(p, "__builtin_alloca requires one integer size argument");
      return (c_value_t){-1, C_BAD, NULL};
    }
    int pointer =
        emit_value(p, (nyr_inst_t){.op = NYIR_INLINE_ASM,
                                   .dst = -1,
                                   .a = size.value,
                                   .symbol = "__nyc_builtin_alloca_dynamic",
                                   .effects = NYIR_EFFECT_ALLOCATION |
                                              NYIR_EFFECT_VOLATILE |
                                              NYIR_EFFECT_UNKNOWN_SIDE_EFFECT,
                                   .scalar_class = NYIR_SCALAR_POINTER});
    if (pointer < 0) {
      if (!p->err || !p->err_len || !p->err[0])
        fail(p, "NYC could not lower dynamic __builtin_alloca");
      return (c_value_t){-1, C_BAD, NULL};
    }
    return (c_value_t){pointer, C_VOID_PTR, NULL};
  }
  if (ny_ctok_eq(name, "__builtin_prefetch")) {
    c_value_t address = parse_expr(p, 1);
    if (address.value < 0 || !c_type_is_pointer(address.type)) {
      fail(p, "__builtin_prefetch requires a pointer argument");
      return (c_value_t){-1, C_BAD, NULL};
    }
    for (unsigned optional = 0; optional < 2 && accept(p, ","); ++optional) {
      c_value_t option = parse_expr(p, 1);
      if (option.value < 0 || !c_type_is_integer(option.type)) {
        fail(p, "__builtin_prefetch options must be integer expressions");
        return (c_value_t){-1, C_BAD, NULL};
      }
    }
    if (!expect(p, ")")) {
      fail(p, "__builtin_prefetch accepts one to three arguments");
      return (c_value_t){-1, C_BAD, NULL};
    }
    return (c_value_t){-1, C_BAD, NULL};
  }
  if (ny_ctok_eq(name, "__builtin_extract_return_addr")) {
    c_value_t address = parse_expr(p, 1);
    if (address.value < 0 || !c_type_is_pointer(address.type) ||
        !expect(p, ")")) {
      fail(p, "__builtin_extract_return_addr requires one pointer argument");
      return (c_value_t){-1, C_BAD, NULL};
    }
    return address;
  }
  if (ny_ctok_eq(name, "__builtin_frame_address")) {
    int64_t level = -1;
    if (!parse_global_const_expr(p, &p->pos, &level) || level != 0 ||
        !expect(p, ")")) {
      fail(p, "__builtin_frame_address currently supports constant level 0");
      return (c_value_t){-1, C_BAD, NULL};
    }
    int value =
        emit_value(p, (nyr_inst_t){.op = NYIR_INLINE_ASM,
                                   .dst = -1,
                                   .a = -1,
                                   .b = -1,
                                   .c = -1,
                                   .d = -1,
                                   .e = -1,
                                   .f = -1,
                                   .symbol = "__nyc_builtin_frame_address_0",
                                   .effects = NYIR_EFFECT_VOLATILE |
                                              NYIR_EFFECT_UNKNOWN_SIDE_EFFECT,
                                   .scalar_class = NYIR_SCALAR_I64_RAW});
    return value < 0 ? (c_value_t){-1, C_BAD, NULL}
                     : (c_value_t){value, C_VOID_PTR, NULL};
  }
  if (ny_ctok_eq(name, "__builtin_return_address")) {
    int64_t level = -1;
    if (!parse_global_const_expr(p, &p->pos, &level) || level != 0 ||
        !expect(p, ")")) {
      fail(p, "__builtin_return_address currently supports constant level 0");
      return (c_value_t){-1, C_BAD, NULL};
    }
    int value =
        emit_value(p, (nyr_inst_t){.op = NYIR_INLINE_ASM,
                                   .dst = -1,
                                   .a = -1,
                                   .b = -1,
                                   .c = -1,
                                   .d = -1,
                                   .e = -1,
                                   .f = -1,
                                   .symbol = "__nyc_builtin_return_address_0",
                                   .effects = NYIR_EFFECT_VOLATILE |
                                              NYIR_EFFECT_UNKNOWN_SIDE_EFFECT,
                                   .scalar_class = NYIR_SCALAR_I64_RAW});
    return value < 0 ? (c_value_t){-1, C_BAD, NULL}
                     : (c_value_t){value, C_VOID_PTR, NULL};
  }
  if (ny_ctok_eq(name, "__builtin_unreachable")) {
    if (!expect(p, ")")) {
      fail(p, "__builtin_unreachable takes no arguments");
      return (c_value_t){-1, C_BAD, NULL};
    }
    if (!emit_noreturn_marker(p))
      return (c_value_t){-1, C_BAD, NULL};
    return (c_value_t){-1, C_BAD, NULL};
  }
  if (ny_ctok_eq(name, "__has_attribute")) {
    ny_ctok_t attribute;
    if (!parse_name(p, &attribute) || !expect(p, ")")) {
      fail(p, "__has_attribute requires one attribute name");
      return (c_value_t){-1, C_BAD, NULL};
    }
    int value =
        emit_i64_constant(p, preproc_attribute_accepted(
                                 (const char *)attribute.start, attribute.len));
    return value < 0 ? (c_value_t){-1, C_BAD, NULL}
                     : (c_value_t){value, C_I32, NULL};
  }
  if (ny_ctok_eq(name, "__builtin_constant_p")) {
    size_t open = p->pos ? p->pos - 1 : p->pos;
    size_t after = skip_balanced(p, open, "(", ")");
    if (after <= open + 2 || after >= p->token_count) {
      fail(p, "__builtin_constant_p requires one argument");
      return (c_value_t){-1, C_BAD, NULL};
    }
    c_parser_t probe = *p;
    char probe_error[1] = {0};
    probe.err = probe_error;
    probe.err_len = sizeof(probe_error);
    size_t constant_end = p->pos;
    int64_t constant_value = 0;
    bool is_constant =
        parse_global_const_expr(&probe, &constant_end, &constant_value) &&
        constant_end + 1 == after && tok_is(&probe, constant_end, ")");
    p->pos = after;
    int result = emit_i64_constant(p, is_constant);
    return result < 0 ? (c_value_t){-1, C_BAD, NULL}
                      : (c_value_t){result, C_I32, NULL};
  }
  if (ny_ctok_eq(name, "__builtin_object_size")) {
    c_parser_t probe = *p;
    nyr_func_t probe_out;
    expression_probe_begin(&probe, &probe_out);
    char probe_error[256] = {0};
    probe.err = probe_error;
    probe.err_len = sizeof(probe_error);
    probe.type_lookahead = true;
    c_value_t object = parse_expr(&probe, 1);
    const c_array_shape_t *shape = array_shape_for_value(&probe, object.value);
    size_t known_size = c_array_shape_size(shape);
    if (!known_size && probe.last_type_is_array)
      known_size = probe.last_type_array_size;
    bool object_ok = object.value >= 0 && c_type_is_pointer(object.type) &&
                     !probe_error[0] && expect(&probe, ",");
    size_t after_object = probe.pos;
    expression_probe_end(&probe);
    p->pos = after_object;
    int64_t mode = -1;
    if (!object_ok || !parse_global_const_expr(p, &p->pos, &mode) || mode < 0 ||
        mode > 3 || !expect(p, ")")) {
      fail(p, "__builtin_object_size requires a pointer expression and "
              "constant mode from 0 through 3");
      return (c_value_t){-1, C_BAD, NULL};
    }
    int64_t result_value = known_size ? (int64_t)known_size : mode < 2 ? -1 : 0;
    int result = emit_i64_constant(p, result_value);
    return result < 0 ? (c_value_t){-1, C_BAD, NULL}
                      : (c_value_t){result, c_size_type(p->abi), NULL};
  }
  if (ny_ctok_eq(name, "__builtin_offsetof")) {
    c_value_t offset;
    if (!parse_builtin_offsetof_runtime_args(p, &offset) || !expect(p, ")")) {
      fail(p, "__builtin_offsetof requires a declared aggregate member");
      return (c_value_t){-1, C_BAD, NULL};
    }
    return offset;
  }
  if (ny_ctok_eq(name, "__builtin_inf") ||
      ny_ctok_eq(name, "__builtin_huge_val") ||
      ny_ctok_eq(name, "__builtin_inff") ||
      ny_ctok_eq(name, "__builtin_huge_valf")) {
    bool is_f32 = ny_ctok_eq(name, "__builtin_inff") ||
                  ny_ctok_eq(name, "__builtin_huge_valf");
    if (!expect(p, ")")) {
      fail(p, "infinity builtin takes no arguments");
      return (c_value_t){-1, C_BAD, NULL};
    }
    int value = emit_value(
        p,
        (nyr_inst_t){
            .op = is_f32 ? NYIR_CONST_F32 : NYIR_CONST_F64,
            .dst = -1,
            .imm = is_f32 ? INT64_C(0x7f800000) : INT64_C(0x7ff0000000000000),
            .scalar_class = is_f32 ? NYIR_SCALAR_F32 : NYIR_SCALAR_F64});
    return value < 0 ? (c_value_t){-1, C_BAD, NULL}
                     : (c_value_t){value, is_f32 ? C_F32 : C_F64, NULL};
  }
  if (ny_ctok_eq(name, "__builtin_isnan")) {
    c_value_t value = parse_expr(p, 1);
    if (value.value < 0 || (value.type != C_F32 && value.type != C_F64) ||
        !expect(p, ")")) {
      fail(p, "__builtin_isnan requires one floating-point argument");
      return (c_value_t){-1, C_BAD, NULL};
    }
    int result = emit_value(
        p, (nyr_inst_t){.op = value.type == C_F32 ? NYIR_CMP_F32 : NYIR_CMP_F64,
                        .dst = -1,
                        .a = value.value,
                        .b = value.value,
                        .cmp = NYIR_CMP_NE,
                        .scalar_class = NYIR_SCALAR_I64_RAW});
    return result < 0 ? (c_value_t){-1, C_BAD, NULL}
                      : (c_value_t){result, C_I32, NULL};
  }
  if (ny_ctok_eq(name, "__builtin_isdigit")) {
    c_value_t value = parse_expr(p, 1);
    if (value.value < 0 || !c_type_is_integer(value.type) || !expect(p, ")")) {
      fail(p, "__builtin_isdigit requires one integer argument");
      return (c_value_t){-1, C_BAD, NULL};
    }
    int byte = normalize_integer(p, C_U8, value.value);
    int lower = emit_i64_constant(p, '0');
    int upper = emit_i64_constant(p, '9');
    int ge = byte >= 0 && lower >= 0
                 ? emit_i64_compare(p, byte, lower, NYIR_CMP_GE)
                 : -1;
    int le = byte >= 0 && upper >= 0
                 ? emit_i64_compare(p, byte, upper, NYIR_CMP_LE)
                 : -1;
    int result =
        ge >= 0 && le >= 0 ? emit_i64_binary(p, NYIR_AND_I64, ge, le) : -1;
    return result < 0 ? (c_value_t){-1, C_BAD, NULL}
                      : (c_value_t){result, C_I32, NULL};
  }
  if (ny_ctok_eq(name, "__sync_synchronize")) {
    if (!expect(p, ")")) {
      fail(p, "__sync_synchronize takes no arguments");
      return (c_value_t){-1, C_BAD, NULL};
    }
    const char *barrier = p->abi == NYC_ABI_AAPCS64 ? "dmb ish" : "mfence";
    if (!emit(p,
              (nyr_inst_t){
                  .op = NYIR_INLINE_ASM, .dst = -1, .a = -1, .symbol = barrier},
              NULL))
      return (c_value_t){-1, C_BAD, NULL};
    return (c_value_t){-1, C_BAD, NULL};
  }
  if (ny_ctok_eq(name, "__atomic_store_n") ||
      ny_ctok_eq(name, "__atomic_load_n") ||
      ny_ctok_eq(name, "__c11_atomic_store") ||
      ny_ctok_eq(name, "__c11_atomic_load"))
    return parse_atomic_builtin(p, name);
  if (ny_ctok_eq(name, "__builtin_bswap16") ||
      ny_ctok_eq(name, "__builtin_bswap32") ||
      ny_ctok_eq(name, "__builtin_bswap64"))
    return parse_byte_swap_builtin(p, name);
  if (ny_ctok_eq(name, "__builtin_clzll") || ny_ctok_eq(name, "__builtin_clzl"))
    return parse_clzll_builtin(p);
  if (ny_ctok_eq(name, "__builtin_clz"))
    return parse_clz_builtin(p);
  if (ny_ctok_eq(name, "__builtin_ctz") || ny_ctok_eq(name, "__builtin_ctzl") ||
      ny_ctok_eq(name, "__builtin_ctzll"))
    return parse_ctzl_builtin(p);
  if (ny_ctok_eq(name, "__builtin_ffs"))
    return parse_ffs_builtin(p);
  if (ny_ctok_eq(name, "__builtin_add_overflow") ||
      ny_ctok_eq(name, "__builtin_sub_overflow") ||
      ny_ctok_eq(name, "__builtin_mul_overflow"))
    return parse_overflow_builtin(p, name);
  bool is_va_start = tok_same(
      name, (ny_ctok_t){.start = "va_start", .len = sizeof("va_start") - 1});
  bool is_builtin_va_start =
      tok_same(name, (ny_ctok_t){.start = "__builtin_va_start",
                                 .len = sizeof("__builtin_va_start") - 1});
  bool is_va_end = tok_same(
      name, (ny_ctok_t){.start = "va_end", .len = sizeof("va_end") - 1});
  bool is_builtin_va_end =
      tok_same(name, (ny_ctok_t){.start = "__builtin_va_end",
                                 .len = sizeof("__builtin_va_end") - 1});
  bool is_va_copy = tok_same(
      name, (ny_ctok_t){.start = "va_copy", .len = sizeof("va_copy") - 1});
  bool is_builtin_va_copy =
      tok_same(name, (ny_ctok_t){.start = "__builtin_va_copy",
                                 .len = sizeof("__builtin_va_copy") - 1});
  if (is_va_start || is_builtin_va_start) {
    c_value_t list = parse_expr(p, 1);
    if (p->abi == NYC_ABI_I386 && list.value >= 0) {
      const c_lvalue_t *lvalue = find_lvalue(p, list.value);
      if (!lvalue || !discard_lvalue_read(p, lvalue)) {
        fail(p, "va_start requires a writable va_list");
        return (c_value_t){-1, C_BAD, NULL};
      }
      list.value = lvalue->address;
      if (lvalue->is_local) {
        c_local_t *local = find_local_slot(p, lvalue->local_slot);
        if (!local || local->is_const)
          return (c_value_t){-1, C_BAD, NULL};
        local->has_integer_constant = false;
        local->address_taken = true;
        if (list.value < 0)
          list.value =
              emit_value(p, (nyr_inst_t){.op = NYIR_ADDR_LOCAL,
                                         .dst = -1,
                                         .imm = lvalue->local_slot,
                                         .scalar_class = NYIR_SCALAR_I64_RAW});
      }
    }
    if (list.value < 0 ||
        (!c_type_is_pointer(list.type) && list.type != C_VA_LIST) ||
        !expect(p, ",") || !tok_ident(*current(p))) {
      fail(p, "va_start requires a va_list and its final named parameter");
      return (c_value_t){-1, C_BAD, NULL};
    }
    ny_ctok_t last_parameter = *current(p);
    ++p->pos;
    if (!expect(p, ")"))
      return (c_value_t){-1, C_BAD, NULL};
    if (!p->function || !p->function->variadic || !p->function->param_count ||
        (p->abi != NYC_ABI_I386 && p->va_save_area < 0) ||
        !tok_same(last_parameter,
                  p->function->params[p->function->param_count - 1].name)) {
      fail(p, "va_start must name the final parameter of a variadic function");
      return (c_value_t){-1, C_BAD, NULL};
    }
    unsigned gp = 0;
    unsigned fp = 0;
    size_t stack_bytes = 0;
    if (p->abi == NYC_ABI_I386)
      stack_bytes = (p->out->param_count - p->function->param_count) * 4;
    if (p->abi == NYC_ABI_SYSV_X86_64 && p->function->result == C_AGG) {
      uint32_t result_packed = 0;
      if (!p->function->result_agg ||
          !nyc_classify_aggregate(p->function->result_agg, p->abi, 0,
                                  &result_packed)) {
        fail(p, "va_start aggregate return type is not supported by the C ABI");
        return (c_value_t){-1, C_BAD, NULL};
      }
      if (nyc_sysv_return_is_memory(result_packed))
        gp = 1;
    }
    for (size_t i = 0; i < p->function->param_count; ++i) {
      if (!c_va_start_account_parameter(p, &p->function->params[i], &gp, &fp,
                                        &stack_bytes)) {
        fail(p, "va_start fixed parameter has an unsupported ABI layout");
        return (c_value_t){-1, C_BAD, NULL};
      }
    }
    if (stack_bytes > (size_t)(INT_MAX - 16)) {
      fail(p, "va_start fixed-parameter stack area is too large");
      return (c_value_t){-1, C_BAD, NULL};
    }
    uint32_t gp_offset = gp * 8;
    uint32_t fp_offset = 48 + fp * 16;
    int64_t offsets = (int64_t)gp_offset | ((int64_t)fp_offset << 16) |
                      ((int64_t)stack_bytes << 32);
    if (!emit(p,
              (nyr_inst_t){
                  .op = NYIR_VA_START,
                  .dst = -1,
                  .a = list.value,
                  .b = p->abi == NYC_ABI_I386 ? list.value : p->va_save_area,
                  .c = -1,
                  .d = -1,
                  .e = -1,
                  .f = -1,
                  .imm = offsets,
                  .flags = 0,
                  .effects = NYIR_EFFECT_READ_MEMORY |
                             NYIR_EFFECT_WRITE_MEMORY | NYIR_EFFECT_MAY_TRAP},
              NULL))
      return (c_value_t){-1, C_BAD, NULL};
    int result =
        emit_value(p, (nyr_inst_t){.op = NYIR_CONST_I64, .dst = -1, .imm = 0});
    return (c_value_t){result, C_I32, NULL};
  }
  if (is_va_end || is_builtin_va_end) {
    c_value_t list = parse_expr(p, 1);
    if (list.value < 0 ||
        (!c_type_is_pointer(list.type) && list.type != C_VA_LIST) ||
        !expect(p, ")")) {
      fail(p, "va_end requires a va_list");
      return (c_value_t){-1, C_BAD, NULL};
    }
    int result =
        emit_value(p, (nyr_inst_t){.op = NYIR_CONST_I64, .dst = -1, .imm = 0});
    return (c_value_t){result, C_I32, NULL};
  }
  if (is_va_copy || is_builtin_va_copy) {
    c_value_t dst = parse_expr(p, 1);
    c_lvalue_t dst_lvalue = {0};
    if (p->abi == NYC_ABI_I386) {
      const c_lvalue_t *lvalue =
          dst.value >= 0 ? find_lvalue(p, dst.value) : NULL;
      if (!lvalue || !discard_lvalue_read(p, lvalue)) {
        fail(p, "va_copy requires a writable va_list");
        return (c_value_t){-1, C_BAD, NULL};
      }
      dst_lvalue = *lvalue;
    }
    if (dst.value < 0 ||
        (!c_type_is_pointer(dst.type) && dst.type != C_VA_LIST) ||
        !expect(p, ",")) {
      fail(p, "va_copy requires two va_lists");
      return (c_value_t){-1, C_BAD, NULL};
    }
    c_value_t src = parse_expr(p, 1);
    if (src.value < 0 ||
        (!c_type_is_pointer(src.type) && src.type != C_VA_LIST) ||
        !expect(p, ")")) {
      fail(p, "va_copy requires two va_lists");
      return (c_value_t){-1, C_BAD, NULL};
    }
    if (p->abi == NYC_ABI_I386
            ? !c_store_i386_va_list(p, &dst_lvalue, src.value)
            : !emit(p,
                    (nyr_inst_t){.op = NYIR_COPY_STRUCT,
                                 .dst = -1,
                                 .a = dst.value,
                                 .b = src.value,
                                 .imm = 24,
                                 .effects = NYIR_EFFECT_READ_MEMORY |
                                            NYIR_EFFECT_WRITE_MEMORY |
                                            NYIR_EFFECT_MAY_TRAP},
                    NULL))
      return (c_value_t){-1, C_BAD, NULL};
    int result =
        emit_value(p, (nyr_inst_t){.op = NYIR_CONST_I64, .dst = -1, .imm = 0});
    return (c_value_t){result, C_I32, NULL};
  }
  c_local_t *callee = find_local(p, name);
  if (callee && callee->type == C_FUNC_PTR) {
    int target = lower_local_load(p, callee);
    p->indirect_bool_params = 0; /* set below only for sources that know it */
    p->indirect_result_type = callee->function_result_type;
    p->indirect_result_agg = callee->agg;
    p->has_indirect_result_signature = true;
    p->indirect_bool_params = callee->function_bool_params;
    return parse_indirect_call(p, (c_value_t){target, C_FUNC_PTR, NULL});
  }
  c_global_t *global_callee = find_global(p, name);
  if (global_callee && global_callee->type == C_FUNC_PTR) {
    static const char extern_prefix[] = "__nyc_extern_";
    static const char data_prefix[] = "__nyc_data_";
    const char *prefix = global_callee->is_external || global_callee->is_weak
                             ? extern_prefix
                             : data_prefix;
    size_t prefix_len = strlen(prefix);
    if (name.len > SIZE_MAX - prefix_len - 1) {
      fail(p, "global function-pointer symbol is too long");
      return (c_value_t){-1, C_BAD, NULL};
    }
    char *symbol = malloc(prefix_len + name.len + 1);
    if (!symbol) {
      fail(p, "out of memory lowering a global function-pointer call");
      return (c_value_t){-1, C_BAD, NULL};
    }
    memcpy(symbol, prefix, prefix_len);
    memcpy(symbol + prefix_len, name.start, name.len);
    symbol[prefix_len + name.len] = '\0';
    int address = emit_value(
        p, (nyr_inst_t){.op = NYIR_ADDR_SYMBOL, .dst = -1, .symbol = symbol});
    free(symbol);
    int target =
        address >= 0
            ? emit_value(p, (nyr_inst_t){.op = NYIR_LOAD_I64,
                                         .dst = -1,
                                         .a = address,
                                         .effects = NYIR_EFFECT_READ_MEMORY |
                                                    NYIR_EFFECT_MAY_TRAP,
                                         .scalar_class = NYIR_SCALAR_I64_RAW})
            : -1;
    p->indirect_bool_params = 0; /* set below only for sources that know it */
    p->indirect_result_type = global_callee->function_result_type;
    p->indirect_result_agg = global_callee->agg;
    p->has_indirect_result_signature = true;
    return parse_indirect_call(p, (c_value_t){target, C_FUNC_PTR, NULL});
  }
  c_function_t *fn = find_function(p, name);
  bool inline_memory_copy =
      (ny_ctok_eq(name, "memcpy") || ny_ctok_eq(name, "__builtin_memcpy")) &&
      (!fn ||
       (!p->no_builtin && !fn->defined && !fn->has_asm_alias && !fn->variadic &&
        fn->result == C_VOID_PTR && fn->param_count == 3 &&
        fn->params[0].type == C_VOID_PTR && fn->params[1].type == C_VOID_PTR &&
        fn->params[2].type == (p->abi == NYC_ABI_I386 ? C_U32 : C_U64)));
  c_function_t builtin;
  if (!fn)
    fn = find_builtin_function(name, p->abi, p->no_builtin, &builtin);
  if (!fn) {
    char message[192];
    snprintf(message, sizeof(message),
             "call target `%.*s` must be declared in this translation unit",
             (int)(name.len > 120 ? 120 : name.len), name.start);
    fail(p, message);
    return (c_value_t){-1, C_BAD, NULL};
  }
  if (fn->has_asm_alias && !fn->asm_alias.len) {
    fail(p, "function asm label must be a string literal");
    return (c_value_t){-1, C_BAD, NULL};
  }
  if (fn->param_count > NYIR_CALL_MAX_ARGS) {
    fail(p, "direct call exceeds the NYIR limit of 64 arguments");
    return (c_value_t){-1, C_BAD, NULL};
  }
  bool empty_agg_result = fn->result == C_AGG && fn->result_agg &&
                          fn->result_agg->complete && fn->result_agg->size == 0;
  bool has_agg_result = fn->result == C_AGG && !empty_agg_result;
  uint32_t result_packed = 0;
  bool sret = false;
  if (has_agg_result) {
    if (!fn->result_agg || !fn->result_agg->complete) {
      fail(p, "call result type must be a complete struct or union");
      return (c_value_t){-1, C_BAD, NULL};
    }
    if (!nyc_classify_aggregate(fn->result_agg, p->abi, 0, &result_packed)) {
      fail(p, "aggregate return type is not supported by the C ABI");
      return (c_value_t){-1, C_BAD, NULL};
    }
    sret = nyc_sysv_return_is_memory(result_packed);
  }
  bool has_agg_param = false;
  for (size_t i = 0; i < fn->param_count; ++i)
    if (fn->params[i].type == C_AGG)
      has_agg_param = true;
  size_t argc = fn->param_count + (sret ? 1u : 0u);
  if (argc > NYIR_CALL_MAX_ARGS) {
    fail(p, "direct call exceeds the NYIR limit of 64 arguments");
    return (c_value_t){-1, C_BAD, NULL};
  }
  nyr_inst_t call = {
      .op = NYIR_CALL,
      .dst = -1,
      .a = -1,
      .b = -1,
      .c = -1,
      .d = -1,
      .e = -1,
      .f = -1,
      .imm = (int64_t)argc,
      .flags = NYIR_INST_F_EXTERN | (sret ? NYIR_INST_F_SRET : 0u),
      .effects = NYIR_EFFECT_CALL,
      .symbol =
          strndup(fn->asm_alias.len ? fn->asm_alias.start : fn->name.start,
                  fn->asm_alias.len ? fn->asm_alias.len : fn->name.len)};
  if (fn->variadic)
    call.flags |= NYIR_INST_F_VARIADIC;
  if (p->abi == NYC_ABI_I386)
    call.flags |= NYIR_INST_F_I386_C_ABI;
  call.flags |= c_function_regparm(p, fn) << NYIR_INST_F_I386_REGPARM_SHIFT;
  if (!call.symbol) {
    fail(p, "out of memory creating call symbol");
    return (c_value_t){-1, C_BAD, NULL};
  }
  int extra_args[NYIR_CALL_MAX_ARGS - 6];
  uint32_t arg_sizes[NYIR_CALL_MAX_ARGS] = {0};
  if (argc > 6 || fn->variadic)
    call.extra_args = extra_args;
  if (has_agg_param || sret || fn->variadic)
    call.arg_sizes = arg_sizes;
  if (sret) {
    call.scalar_class = NYIR_SCALAR_I64_RAW;
  } else if (has_agg_result) {
    call.scalar_class =
        (NYIR_ARG_AGG_CLASS(result_packed, 0) == NYIR_ARG_CLASS_SSE ||
         NYIR_ARG_AGG_CLASS(result_packed, 0) == NYIR_ARG_CLASS_HFA_F64)
            ? NYIR_SCALAR_F64
            : NYIR_SCALAR_I64_RAW;
    call.ret_aggregate_size = NYIR_ARG_AGG_SIZE(result_packed);
    call.ret_aggregate_classes[0] =
        (uint8_t)NYIR_ARG_AGG_CLASS(result_packed, 0);
    call.ret_aggregate_classes[1] =
        (uint8_t)NYIR_ARG_AGG_CLASS(result_packed, 1);
  } else if (empty_agg_result) {
    call.scalar_class = NYIR_SCALAR_UNKNOWN;
  } else {
    call.scalar_class = c_type_is_float(fn->result)
                            ? c_float_scalar_class(fn->result)
                        : fn->result == C_BAD ? NYIR_SCALAR_UNKNOWN
                                              : NYIR_SCALAR_I64_RAW;
  }
  int result_addr = -1;
  if (sret) {
    /*
     * MEMORY-class result: allocate the caller-side return buffer and pass
     * its address as the hidden first argument.
     */
    size_t offset = 0;
    if (!agg_area_alloc(p, fn->result_agg->size, &offset)) {
      free((void *)call.symbol);
      return (c_value_t){-1, C_BAD, NULL};
    }
    result_addr = lower_add_const(p, p->agg_base, offset);
    if (result_addr < 0) {
      free((void *)call.symbol);
      return (c_value_t){-1, C_BAD, NULL};
    }
    int *args[] = {&call.a, &call.b, &call.c, &call.d, &call.e, &call.f};
    *args[0] = result_addr;
    nyr_call_arg_class_set(call.arg_scalar_classes, 0, NYIR_SCALAR_I64_RAW);
  }
  size_t argument = sret ? 1 : 0;
  size_t logical = 0;
  if (!accept(p, ")")) {
    for (;;) {
      bool variadic_arg = logical >= fn->param_count;
      if (variadic_arg && !fn->variadic) {
        fail(p, "direct call argument count does not match its prototype");
        goto call_error;
      }
      if (argument >= NYIR_CALL_MAX_ARGS) {
        fail(p, "direct call exceeds the NYIR limit of 64 arguments");
        goto call_error;
      }
      size_t argument_start = p->pos;
      c_value_t value = parse_expr(p, 1);
      c_param_t *param = variadic_arg ? NULL : &fn->params[logical];
      int *args[] = {&call.a, &call.b, &call.c, &call.d, &call.e, &call.f};
      if (variadic_arg) {
        if (value.value < 0 || value.type == C_BAD) {
          fail(p, "variadic arguments require supported scalar values");
          goto call_error;
        }
        if (value.type == C_AGG) {
          uint32_t packed = 0;
          if (!value.agg ||
              !nyc_classify_argument(value.agg, p->abi, 0, &packed)) {
            fail(p, "aggregate argument type is not supported by the C ABI");
            goto call_error;
          }
          call.arg_sizes[argument] = packed;
          nyr_call_arg_class_set(call.arg_scalar_classes, argument,
                                 NYIR_SCALAR_AGGREGATE);
        } else if (value.type == C_F32) {
          value.value =
              emit_value(p, (nyr_inst_t){.op = NYIR_F32_TO_F64,
                                         .dst = -1,
                                         .a = value.value,
                                         .scalar_class = NYIR_SCALAR_F64});
          if (value.value < 0)
            goto call_error;
          value.type = C_F64;
        } else if (value.type == C_U8 || value.type == C_CHAR ||
                   value.type == C_U16 || value.type == C_I16) {
          value.value = normalize_integer(p, C_I32, value.value);
          if (value.value < 0)
            goto call_error;
          value.type = C_I32;
        }
        nyr_call_arg_class_set(call.arg_scalar_classes, argument,
                               c_type_is_float(value.type)
                                   ? c_float_scalar_class(value.type)
                                   : NYIR_SCALAR_I64_RAW);
      } else if (param->type == C_AGG) {
        bool transparent_pointer =
            value.value >= 0 && param->agg &&
            transparent_union_accepts_pointer(
                param->agg, value.type, p->last_value_null_pointer_constant);
        if (transparent_pointer) {
          int storage =
              emit_value(p, (nyr_inst_t){.op = NYIR_ALLOCA,
                                         .dst = -1,
                                         .imm = (int64_t)param->agg->size,
                                         .scalar_class = NYIR_SCALAR_I64_RAW});
          if (storage < 0 ||
              !emit(p,
                    (nyr_inst_t){.op = NYIR_STORE_I64,
                                 .dst = -1,
                                 .a = storage,
                                 .c = value.value,
                                 .effects = NYIR_EFFECT_WRITE_MEMORY |
                                            NYIR_EFFECT_MAY_TRAP,
                                 .scalar_class = NYIR_SCALAR_I64_RAW},
                    NULL))
            goto call_error;
          value.type = C_AGG;
          value.agg = param->agg;
          value.value = storage;
        }
        bool matching_aggregate =
            value.agg == param->agg ||
            (value.agg && param->agg && value.agg->is_int128 &&
             param->agg->is_int128);
        if (value.value < 0 || value.type != C_AGG || !matching_aggregate) {
          fail(p, "direct calls require matching aggregate argument types");
          goto call_error;
        }
        uint32_t packed = 0;
        if (!nyc_classify_argument(param->agg, p->abi, 0, &packed)) {
          fail(p, "aggregate argument type is not supported by the C ABI");
          goto call_error;
        }
        call.arg_sizes[argument] = packed;
        nyr_call_arg_class_set(call.arg_scalar_classes, argument,
                               NYIR_SCALAR_AGGREGATE);
      } else {
        if (value.value < 0 ||
            !(value.type == param->type ||
              c_pointer_compatible(value.type, param->type) ||
              (c_type_is_integer(value.type) &&
               c_type_is_integer(param->type)) ||
              (c_type_is_integer(value.type) && c_type_is_float(param->type)) ||
              (c_type_is_float(value.type) && c_type_is_float(param->type)) ||
              (param->is_bool && c_type_is_pointer(value.type)) ||
              (c_type_is_pointer(param->type) &&
               p->last_value_null_pointer_constant) ||
              (value.type == C_AGG_ARRAY_PTR && param->type == C_AGG_PTR &&
               value.agg == param->agg) ||
              /*
               * A rank-one array lvalue decays to a pointer to its first
               * element when passed to an adjusted array parameter.  Kernel
               * code such as `cpumask_var_t *pmasks[]` relies on this when
               * the parameter is defined as `cpumask_var_t mask[1]`.
               */
              (value.type == C_AGG && param->type == C_AGG_PTR &&
               value.agg == param->agg) ||
              (getenv("NYC_KERNEL_COMPAT") &&
               ((c_type_is_pointer(value.type) &&
                 c_type_is_pointer(param->type)) ||
                (c_type_is_pointer(value.type) &&
                 c_type_is_integer(param->type)) ||
                (c_type_is_integer(value.type) &&
                 c_type_is_pointer(param->type)))))) {
          char message[512];
          char expression[160] = {0};
          size_t expression_len = 0;
          for (size_t i = argument_start; i < p->pos && i < argument_start + 16;
               ++i) {
            if (expression_len + p->tokens[i].len + 2 >= sizeof(expression))
              break;
            if (expression_len)
              expression[expression_len++] = ' ';
            memcpy(expression + expression_len, p->tokens[i].start,
                   p->tokens[i].len);
            expression_len += p->tokens[i].len;
          }
          expression[expression_len] = '\0';
          snprintf(message, sizeof(message),
                   "`%.*s` argument %zu (`%.*s`) has type %d, expected %d "
                   "(expression starts `%s`)",
                   (int)name.len, name.start, logical + 1, (int)param->name.len,
                   param->name.start, value.type, param->type, expression);
          fail(p, message);
          goto call_error;
        }
        if (value.type == C_AGG_ARRAY_PTR && param->type == C_AGG_PTR &&
            value.agg == param->agg)
          value.type = param->type;
        if (value.type == C_AGG && param->type == C_AGG_PTR &&
            value.agg == param->agg)
          value.type = param->type;
        /*
         * A _Bool parameter is a one-byte 0/1 value: converting any non-zero
         * integer argument to it must yield 1, not its low byte
         * (`f(flags & 0x200)` passed 0x200, which the callee read as 0).
         */
        if (param->is_bool && !c_type_is_float(value.type) &&
            (c_type_is_pointer(value.type) || c_type_is_integer(value.type))) {
          int zero = emit_value(
              p, (nyr_inst_t){.op = NYIR_CONST_I64, .dst = -1, .imm = 0});
          value.value = zero < 0
                            ? -1
                            : emit_value(p, (nyr_inst_t){.op = NYIR_CMP_I64,
                                                         .dst = -1,
                                                         .a = value.value,
                                                         .b = zero,
                                                         .cmp = NYIR_CMP_NE});
          if (value.value < 0)
            goto call_error;
          value.type = param->type;
        }
        if (c_type_is_integer(value.type) && c_type_is_float(param->type)) {
          value.value = emit_value(
              p,
              (nyr_inst_t){.op = param->type == C_F32 ? NYIR_I64_TO_F32
                                                      : NYIR_I64_TO_F64,
                           .dst = -1,
                           .a = value.value,
                           .scalar_class = c_float_scalar_class(param->type)});
          if (value.value < 0)
            goto call_error;
          value.type = param->type;
        }
        if (c_type_is_float(value.type) && c_type_is_float(param->type) &&
            value.type != param->type) {
          value.value = emit_value(
              p,
              (nyr_inst_t){.op = param->type == C_F32 ? NYIR_F64_TO_F32
                                                      : NYIR_F32_TO_F64,
                           .dst = -1,
                           .a = value.value,
                           .scalar_class = c_float_scalar_class(param->type)});
          if (value.value < 0)
            goto call_error;
          value.type = param->type;
        }
        if (c_type_is_integer(param->type) && value.type != param->type) {
          value.value = normalize_integer(p, param->type, value.value);
          if (value.value < 0)
            goto call_error;
          value.type = param->type;
        }
        nyr_call_arg_class_set(call.arg_scalar_classes, argument,
                               c_type_is_float(value.type)
                                   ? c_float_scalar_class(value.type)
                                   : NYIR_SCALAR_I64_RAW);
      }
      if (p->abi == NYC_ABI_I386 && c_type_is_integer(value.type) &&
          c_type_size(p->abi, value.type) == 8)
        call.i386_wide_args |= UINT64_C(1) << argument;
      if (argument < sizeof(args) / sizeof(args[0]))
        *args[argument] = value.value;
      else
        call.extra_args[argument - sizeof(args) / sizeof(args[0])] =
            value.value;
      if (argument >= sizeof(args) / sizeof(args[0]))
        call.extra_args_len = argument - sizeof(args) / sizeof(args[0]) + 1;
      ++argument;
      ++logical;
      if (accept(p, ")"))
        break;
      if (!expect(p, ","))
        break;
    }
  }
  if ((!fn->variadic && logical != fn->param_count) ||
      (fn->variadic && logical < fn->param_count)) {
    fail(p, "direct call argument count does not match its prototype");
    goto call_error;
  }
  int64_t copy_size = 0;
  if (inline_memory_copy && c_ir_nonnegative_constant(p, call.c, &copy_size) &&
      copy_size <= 64) {
    if (copy_size && !emit(p,
                           (nyr_inst_t){.op = NYIR_COPY_STRUCT,
                                        .dst = -1,
                                        .a = call.a,
                                        .b = call.b,
                                        .imm = copy_size,
                                        .effects = NYIR_EFFECT_READ_MEMORY |
                                                   NYIR_EFFECT_WRITE_MEMORY |
                                                   NYIR_EFFECT_MAY_TRAP},
                           NULL)) {
      free((void *)call.symbol);
      return (c_value_t){-1, C_BAD, NULL};
    }
    free((void *)call.symbol);
    return (c_value_t){call.a, C_VOID_PTR, NULL};
  }
  call.imm = (int64_t)argument;
  int result_id = -1;
  bool emitted;
  if (fn->no_return) {
    call.dst = -1;
    emitted = emit(p, call, NULL) && emit_noreturn_marker(p);
  } else if (!sret && !has_agg_result &&
             (fn->result == C_BAD || empty_agg_result)) {
    emitted = emit(p, call, NULL);
    if (emitted && empty_agg_result) {
      result_id = emit_value(
          p, (nyr_inst_t){.op = NYIR_CONST_I64, .dst = -1, .imm = 0});
      emitted = result_id >= 0;
    }
  } else {
    emitted = (result_id = emit_value(p, call)) >= 0;
  }
  /*
   * nyr_emit copies borrowed symbols and argument metadata.
   */
  free((void *)call.symbol);
  if (!emitted)
    return (c_value_t){-1, C_BAD, NULL};
  if (fn->no_return)
    return (c_value_t){-1, C_BAD, NULL};
  if (c_type_is_integer(fn->result)) {
    result_id = normalize_integer(p, fn->result, result_id);
    if (result_id < 0)
      return (c_value_t){-1, C_BAD, NULL};
  }
  if (has_agg_result && !sret) {
    return lower_aggregate_call_result(p, result_id, fn->result_agg,
                                       result_packed);
  }
  if (sret)
    return (c_value_t){result_id, C_AGG, fn->result_agg};
  if (empty_agg_result)
    return (c_value_t){result_id, C_AGG, fn->result_agg};
  return (c_value_t){result_id, fn->result,
                     fn->result == C_AGG_PTR || fn->result == C_AGG_PTR_PTR
                         ? fn->result_agg
                         : NULL};

call_error:
  free((void *)call.symbol);
  return (c_value_t){-1, C_BAD, NULL};
}

static c_value_t parse_indirect_call(c_parser_t *p, c_value_t target) {
  int args[NYIR_CALL_MAX_ARGS];
  nyr_scalar_class_t classes[NYIR_CALL_MAX_ARGS];
  uint64_t wide_args = 0;
  size_t argc = 1;
  bool has_result_signature = p->has_indirect_result_signature;
  c_type_t indirect_result_type = p->indirect_result_type;
  const c_agg_type_t *indirect_result_agg = p->indirect_result_agg;
  p->has_indirect_result_signature = false;
  p->indirect_result_agg = NULL;
  uint64_t bool_params = has_result_signature ? p->indirect_bool_params : 0;
  p->indirect_bool_params = 0;
  size_t first_param_arg = 0;
  if (target.value < 0 || !c_type_is_pointer(target.type)) {
    fail(p, "indirect C call requires a function pointer");
    return (c_value_t){-1, C_BAD, NULL};
  }
  bool empty_agg_result =
      indirect_result_type == C_AGG && indirect_result_agg &&
      indirect_result_agg->complete && indirect_result_agg->size == 0;
  bool has_agg_result = indirect_result_type == C_AGG && !empty_agg_result;
  uint32_t result_packed = 0;
  bool sret = false;
  if (has_agg_result) {
    if (!indirect_result_agg || !indirect_result_agg->complete) {
      fail(p, "indirect call result type must be a complete struct or union");
      return (c_value_t){-1, C_BAD, NULL};
    }
    if (!nyc_classify_aggregate(indirect_result_agg, p->abi, 0,
                                &result_packed)) {
      fail(p, "indirect aggregate return type is not supported by the C ABI");
      return (c_value_t){-1, C_BAD, NULL};
    }
    sret = nyc_sysv_return_is_memory(result_packed);
  }
  args[0] = target.value;
  classes[0] = NYIR_SCALAR_I64_RAW;
  int result_addr = -1;
  if (sret) {
    size_t offset = 0;
    if (!agg_area_alloc(p, indirect_result_agg->size, &offset))
      return (c_value_t){-1, C_BAD, NULL};
    result_addr = lower_add_const(p, p->agg_base, offset);
    if (result_addr < 0)
      return (c_value_t){-1, C_BAD, NULL};
    args[argc] = result_addr;
    classes[argc++] = NYIR_SCALAR_I64_RAW;
  }
  first_param_arg = argc;
  if (!accept(p, ")")) {
    for (;;) {
      if (argc == NYIR_CALL_MAX_ARGS) {
        fail(p, "indirect call exceeds the NYIR limit of 63 arguments");
        return (c_value_t){-1, C_BAD, NULL};
      }
      c_value_t value = parse_expr(p, 1);
      if (value.value < 0)
        return (c_value_t){-1, C_BAD, NULL};
      /* A _Bool parameter receives 0 or 1, not the low byte of the value. */
      size_t param_index = argc - first_param_arg;
      if (param_index < 64 && (bool_params >> param_index & 1u) &&
          (c_type_is_integer(value.type) || c_type_is_pointer(value.type) ||
           c_type_is_float(value.type))) {
        int boolean = lower_boolean_value(p, value);
        if (boolean < 0)
          return (c_value_t){-1, C_BAD, NULL};
        value = (c_value_t){boolean, C_I64, NULL};
      }
      args[argc] = value.value;
      if (p->abi == NYC_ABI_I386 && c_type_is_integer(value.type) &&
          c_type_size(p->abi, value.type) == 8)
        wide_args |= UINT64_C(1) << argc;
      classes[argc] = c_type_is_float(value.type)
                          ? c_float_scalar_class(value.type)
                          : NYIR_SCALAR_I64_RAW;
      ++argc;
      if (accept(p, ")"))
        break;
      if (!expect(p, ","))
        return (c_value_t){-1, C_BAD, NULL};
    }
  }
  nyr_inst_t call = {
      .op = NYIR_CALL,
      .dst = -1,
      .imm = (int64_t)argc,
      .flags = NYIR_INST_F_INDIRECT | (sret ? NYIR_INST_F_SRET : 0u) |
               (c_function_regparm(p, NULL) << NYIR_INST_F_I386_REGPARM_SHIFT),
      .effects = NYIR_EFFECT_CALL,
      .scalar_class =
          has_result_signature && c_type_is_float(indirect_result_type)
              ? c_float_scalar_class(indirect_result_type)
              : NYIR_SCALAR_I64_RAW};
  if (has_agg_result && !sret) {
    call.scalar_class =
        (NYIR_ARG_AGG_CLASS(result_packed, 0) == NYIR_ARG_CLASS_SSE ||
         NYIR_ARG_AGG_CLASS(result_packed, 0) == NYIR_ARG_CLASS_HFA_F64)
            ? NYIR_SCALAR_F64
            : NYIR_SCALAR_I64_RAW;
    call.ret_aggregate_size = NYIR_ARG_AGG_SIZE(result_packed);
    call.ret_aggregate_classes[0] =
        (uint8_t)NYIR_ARG_AGG_CLASS(result_packed, 0);
    call.ret_aggregate_classes[1] =
        (uint8_t)NYIR_ARG_AGG_CLASS(result_packed, 1);
  } else if (empty_agg_result) {
    call.scalar_class = NYIR_SCALAR_UNKNOWN;
  }
  call.i386_wide_args = wide_args;
  if (p->abi == NYC_ABI_I386)
    call.flags |= NYIR_INST_F_I386_C_ABI;
  for (size_t i = 0; i < argc; ++i) {
    nyr_call_arg_class_set(call.arg_scalar_classes, i, classes[i]);
    int *inline_args[] = {&call.a, &call.b, &call.c, &call.d, &call.e, &call.f};
    if (i < sizeof(inline_args) / sizeof(inline_args[0]))
      *inline_args[i] = args[i];
    else
      call.extra_args_len = argc - 6;
  }
  if (argc > 6)
    call.extra_args = args + 6;
  if (has_result_signature && indirect_result_type == C_BAD) {
    if (!emit(p, call, NULL))
      return (c_value_t){-1, C_BAD, NULL};
    p->last_value_null_pointer_constant = false;
    return (c_value_t){-1, C_BAD, NULL};
  }
  int result = emit_value(p, call);
  if (result < 0)
    return (c_value_t){-1, C_BAD, NULL};
  p->last_value_null_pointer_constant = false;
  c_type_t result_type = has_result_signature ? indirect_result_type : C_I32;
  if (c_type_is_integer(result_type)) {
    result = normalize_integer(p, result_type, result);
    if (result < 0)
      return (c_value_t){-1, C_BAD, NULL};
  }
  if (has_agg_result && !sret) {
    return lower_aggregate_call_result(p, result, indirect_result_agg,
                                       result_packed);
  }
  if (sret)
    return (c_value_t){result_addr, C_AGG, indirect_result_agg};
  if (empty_agg_result)
    return (c_value_t){result, C_AGG, indirect_result_agg};
  return (c_value_t){result, result_type,
                     result_type == C_AGG_PTR || result_type == C_AGG_PTR_PTR
                         ? indirect_result_agg
                         : NULL};
}

static c_value_t lower_address_aggregate_members(c_parser_t *p,
                                                 c_value_t base) {
  while (tok_is(p, p->pos, ".") || tok_is(p, p->pos, "->")) {
    bool arrow = accept(p, "->");
    if (!arrow)
      (void)expect(p, ".");
    if (base.type != C_AGG && base.type != C_AGG_PTR &&
        base.type != C_AGG_ARRAY_PTR) {
      fail(p, "member address requires an aggregate object");
      return (c_value_t){-1, C_BAD, NULL};
    }
    ny_ctok_t field_name;
    if (!parse_name(p, &field_name))
      return (c_value_t){-1, C_BAD, NULL};
    const c_agg_field_t *field =
        base.agg ? find_field(base.agg, field_name) : NULL;
    if (!field) {
      fail(p, "unknown aggregate field in address expression");
      return (c_value_t){-1, C_BAD, NULL};
    }
    int address = field->offset ? lower_add_const(p, base.value, field->offset)
                                : base.value;
    if (address < 0)
      return (c_value_t){-1, C_BAD, NULL};
    if (field->kind == C_AGG_FIELD_AGG) {
      c_agg_type_t *field_agg = complete_tag(p, (c_agg_type_t *)field->agg);
      base =
          (c_value_t){address, C_AGG_PTR, field_agg ? field_agg : field->agg};
      continue;
    }
    if (field->kind == C_AGG_FIELD_PTR && accept(p, "[")) {
      c_value_t index = parse_expr(p, 1);
      if (index.value < 0 || !c_type_is_integer(index.type) ||
          !expect(p, "]")) {
        fail(p, "aggregate pointer field index must be an integer");
        return (c_value_t){-1, C_BAD, NULL};
      }
      int pointer = lower_load_pointer_field(p, address, field->is_volatile);
      if (field->pointer_to_aggregate_array) {
        if (pointer < 0 || !field->elem_size || !field->agg)
          return (c_value_t){-1, C_BAD, NULL};
        int stride =
            emit_value(p, (nyr_inst_t){.op = NYIR_CONST_I64,
                                       .dst = -1,
                                       .imm = (int64_t)field->elem_size});
        int offset =
            stride < 0
                ? -1
                : emit_value(p,
                             (nyr_inst_t){.op = NYIR_MUL_I64,
                                          .dst = -1,
                                          .a = index.value,
                                          .b = stride,
                                          .flags = c_pointer_offset_flags(p)});
        address =
            offset < 0
                ? -1
                : emit_value(p,
                             (nyr_inst_t){.op = NYIR_ADD_I64,
                                          .dst = -1,
                                          .a = pointer,
                                          .b = offset,
                                          .flags = c_pointer_offset_flags(p)});
        return address < 0 ? (c_value_t){-1, C_BAD, NULL}
                           : (c_value_t){address, C_AGG_ARRAY_PTR, field->agg};
      }
      size_t stride = field->elem_pointer || field->pointer_depth > 1
                          ? c_type_size(p->abi, C_VOID_PTR)
                      : field->agg ? field->agg->size
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
        return (c_value_t){-1, C_BAD, NULL};
      if (field->elem_pointer && tok_is(p, p->pos, "[")) {
        int pointer_value =
            emit_value(p, (nyr_inst_t){.op = NYIR_LOAD_I64,
                                       .dst = -1,
                                       .a = address,
                                       .effects = NYIR_EFFECT_READ_MEMORY |
                                                  NYIR_EFFECT_MAY_TRAP,
                                       .scalar_class = NYIR_SCALAR_I64_RAW});
        c_type_t pointer_value_type =
            field->pointer_element_type != C_BAD
                ? (c_type_t)field->pointer_element_type
            : field->agg ? C_AGG_PTR
                         : C_OPAQUE_PTR;
        while (pointer_value >= 0 && accept(p, "[")) {
          c_value_t element_index = parse_expr(p, 1);
          if (element_index.value < 0 ||
              !c_type_is_integer(element_index.type) || !expect(p, "]")) {
            fail(p, "nested aggregate pointer index must be an integer");
            return (c_value_t){-1, C_BAD, NULL};
          }
          address = lower_pointer_index_address(
              p, pointer_value_type, pointer_value, element_index.value);
          c_type_t element_type =
              c_remove_pointer_levels(pointer_value_type, 1);
          if (address < 0 || element_type == C_BAD)
            return (c_value_t){-1, C_BAD, NULL};
          if (!tok_is(p, p->pos, "[")) {
            c_type_t result_pointer = c_add_pointer_levels(element_type, 1);
            return result_pointer == C_BAD
                       ? (c_value_t){-1, C_BAD, NULL}
                       : (c_value_t){address, result_pointer, NULL};
          }
          pointer_value =
              emit_value(p, (nyr_inst_t){.op = NYIR_LOAD_I64,
                                         .dst = -1,
                                         .a = address,
                                         .effects = NYIR_EFFECT_READ_MEMORY |
                                                    NYIR_EFFECT_MAY_TRAP,
                                         .scalar_class = NYIR_SCALAR_I64_RAW});
          pointer_value_type = element_type;
        }
      }
      c_type_t pointer_type =
          field->pointer_element_type != C_BAD
              ? c_add_pointer_levels((c_type_t)field->pointer_element_type, 1)
          : field->agg && field->pointer_depth > 2 ? C_OPAQUE_PTR_PTR_PTR
          : field->agg ? (field->elem_pointer ? C_AGG_PTR_PTR : C_AGG_PTR)
          : field->elem_char_pointer ? C_CHAR_PTR_PTR
          : field->elem_pointer      ? C_OPAQUE_PTR_PTR
          : field->elem_size == 1
              ? (field->is_signed_byte ? C_I8_PTR : C_CHAR_PTR)
          : field->elem_size == 4 ? C_I32_PTR
          : field->elem_size == 2
              ? (field->elem_unsigned ? C_U16_PTR : C_I16_PTR)
              : C_I64_PTR;
      if (pointer_type == C_BAD)
        return (c_value_t){-1, C_BAD, NULL};
      if (field->agg && (tok_is(p, p->pos, "->") ||
                         (!field->elem_pointer && tok_is(p, p->pos, ".")))) {
        int member_base =
            field->elem_pointer
                ? lower_load_pointer_field(p, address, field->is_volatile)
                : address;
        if (member_base < 0)
          return (c_value_t){-1, C_BAD, NULL};
        base = (c_value_t){member_base, C_AGG_PTR, field->agg};
        continue;
      }
      return (c_value_t){address, pointer_type, field->agg};
    }
    if (field->kind == C_AGG_FIELD_ARRAY && field->agg && accept(p, "[")) {
      c_value_t index = parse_expr(p, 1);
      if (index.value < 0 || !c_type_is_integer(index.type) ||
          !expect(p, "]")) {
        fail(p, "aggregate array address index must be an integer");
        return (c_value_t){-1, C_BAD, NULL};
      }
      size_t element_stride = field->elem_pointer
                                  ? c_type_size(p->abi, C_VOID_PTR)
                                  : field->agg->size;
      for (unsigned dimension = 1; dimension < field->array_rank; ++dimension) {
        if (field->array_extent[dimension] &&
            element_stride > SIZE_MAX / field->array_extent[dimension]) {
          fail(p, "aggregate array row stride overflows");
          return (c_value_t){-1, C_BAD, NULL};
        }
        element_stride *= field->array_extent[dimension];
      }
      int stride = emit_value(p, (nyr_inst_t){.op = NYIR_CONST_I64,
                                              .dst = -1,
                                              .imm = (int64_t)element_stride});
      int offset =
          stride < 0
              ? -1
              : emit_value(p, (nyr_inst_t){.op = NYIR_MUL_I64,
                                           .dst = -1,
                                           .a = index.value,
                                           .b = stride,
                                           .flags = c_pointer_offset_flags(p)});
      int element =
          offset < 0
              ? -1
              : emit_value(p, (nyr_inst_t){.op = NYIR_ADD_I64,
                                           .dst = -1,
                                           .a = address,
                                           .b = offset,
                                           .flags = c_pointer_offset_flags(p)});
      if (element < 0)
        return (c_value_t){-1, C_BAD, NULL};
      if (field->elem_pointer && tok_is(p, p->pos, "->")) {
        int member_base =
            lower_load_pointer_field(p, element, field->is_volatile);
        if (member_base < 0)
          return (c_value_t){-1, C_BAD, NULL};
        base = (c_value_t){member_base, C_AGG_PTR, field->agg};
        continue;
      }
      if (field->elem_pointer)
        return (c_value_t){element, C_AGG_PTR_PTR, field->agg};
      if (!tok_is(p, p->pos, ".") && !tok_is(p, p->pos, "->"))
        return (c_value_t){element, C_AGG_PTR, field->agg};
      base = (c_value_t){element, C_AGG, field->agg};
      continue;
    }
    if (field->kind == C_AGG_FIELD_ARRAY && !field->agg && accept(p, "[")) {
      c_value_t index = parse_expr(p, 1);
      if (index.value < 0 || !c_type_is_integer(index.type) ||
          !expect(p, "]")) {
        fail(p, "aggregate array address index must be an integer");
        return (c_value_t){-1, C_BAD, NULL};
      }
      size_t element_stride = field->elem_size;
      for (unsigned dimension = 1; dimension < field->array_rank; ++dimension) {
        if (field->array_extent[dimension] &&
            element_stride > SIZE_MAX / field->array_extent[dimension]) {
          fail(p, "aggregate array row stride overflows");
          return (c_value_t){-1, C_BAD, NULL};
        }
        element_stride *= field->array_extent[dimension];
      }
      int stride = emit_value(p, (nyr_inst_t){.op = NYIR_CONST_I64,
                                              .dst = -1,
                                              .imm = (int64_t)element_stride});
      int offset =
          stride < 0
              ? -1
              : emit_value(p, (nyr_inst_t){.op = NYIR_MUL_I64,
                                           .dst = -1,
                                           .a = index.value,
                                           .b = stride,
                                           .flags = c_pointer_offset_flags(p)});
      address =
          offset < 0
              ? -1
              : emit_value(p, (nyr_inst_t){.op = NYIR_ADD_I64,
                                           .dst = -1,
                                           .a = address,
                                           .b = offset,
                                           .flags = c_pointer_offset_flags(p)});
      if (address < 0)
        return (c_value_t){-1, C_BAD, NULL};
      if (field->elem_pointer) {
        int pointer = lower_load_pointer_field(p, address, field->is_volatile);
        c_type_t pointer_type = field->pointer_element_type != C_BAD
                                    ? (c_type_t)field->pointer_element_type
                                    : C_OPAQUE_PTR;
        while (pointer >= 0 && accept(p, "[")) {
          c_value_t nested_index = parse_expr(p, 1);
          if (nested_index.value < 0 || !c_type_is_integer(nested_index.type) ||
              !expect(p, "]")) {
            fail(p, "nested aggregate pointer index must be an integer");
            return (c_value_t){-1, C_BAD, NULL};
          }
          c_type_t element_type = c_remove_pointer_levels(pointer_type, 1);
          if (element_type == C_BAD) {
            fail(p, "nested aggregate pointer index has an unsupported type");
            return (c_value_t){-1, C_BAD, NULL};
          }
          address = lower_pointer_index_address(p, pointer_type, pointer,
                                                nested_index.value);
          if (address < 0)
            return (c_value_t){-1, C_BAD, NULL};
          pointer_type = element_type;
          if (c_type_is_pointer(pointer_type) && tok_is(p, p->pos, "[")) {
            pointer = emit_value(
                p, (nyr_inst_t){.op = NYIR_LOAD_I64,
                                .dst = -1,
                                .a = address,
                                .effects = NYIR_EFFECT_READ_MEMORY |
                                           NYIR_EFFECT_MAY_TRAP,
                                .scalar_class = NYIR_SCALAR_I64_RAW});
          } else {
            pointer = -1;
          }
        }
        c_type_t result_type = c_add_pointer_levels(pointer_type, 1);
        return result_type == C_BAD
                   ? (c_value_t){-1, C_BAD, NULL}
                   : (c_value_t){address, result_type, field->agg};
      }
      c_type_t pointer_type =
          field->elem_f32   ? C_F32_PTR
          : field->elem_f64 ? C_F64_PTR
          : field->elem_size == 1
              ? (field->is_signed_byte ? C_I8_PTR : C_CHAR_PTR)
          : field->elem_size == 2
              ? (field->elem_unsigned ? C_U16_PTR : C_I16_PTR)
          : field->elem_size == 4
              ? (field->elem_unsigned ? C_U32_PTR : C_I32_PTR)
          : field->elem_unsigned ? C_U64_PTR
                                 : C_I64_PTR;
      return (c_value_t){address, pointer_type, NULL};
    }
    if (field->kind == C_AGG_FIELD_PTR && field->agg &&
        tok_is(p, p->pos, "->")) {
      int pointer = lower_load_pointer_field(p, address, field->is_volatile);
      if (pointer < 0)
        return (c_value_t){-1, C_BAD, NULL};
      base = (c_value_t){pointer, C_AGG_PTR, field->agg};
      continue;
    }
    c_type_t pointer_type =
        field->kind == C_AGG_FIELD_ARRAY
            ? (field->agg                     ? C_AGG_PTR
               : field->elem_function_pointer ? C_FUNC_PTR_PTR
               : field->elem_pointer || field->elem_char_pointer
                   ? C_OPAQUE_PTR_PTR
               : field->elem_f32 ? C_F32_PTR
               : field->elem_f64 ? C_F64_PTR
               : field->elem_size == 1
                   ? (field->is_signed_byte ? C_I8_PTR : C_CHAR_PTR)
               : field->elem_size == 2
                   ? (field->elem_unsigned ? C_U16_PTR : C_I16_PTR)
               : field->elem_size == 4
                   ? (field->elem_unsigned ? C_U32_PTR : C_I32_PTR)
               : field->elem_unsigned ? C_U64_PTR
                                      : C_I64_PTR)
        : field->kind == C_AGG_FIELD_I32 ? C_I32_PTR
        : field->kind == C_AGG_FIELD_U32 ? C_U32_PTR
        : field->kind == C_AGG_FIELD_U8
            ? (field->is_signed_byte ? C_I8_PTR : C_CHAR_PTR)
        : field->kind == C_AGG_FIELD_I16 ? C_I16_PTR
        : field->kind == C_AGG_FIELD_U16 ? C_U16_PTR
        : field->kind == C_AGG_FIELD_I64 ? C_I64_PTR
        : field->kind == C_AGG_FIELD_U64 ? C_U64_PTR
        : field->kind == C_AGG_FIELD_F32 ? C_F32_PTR
        : field->kind == C_AGG_FIELD_F64 ? C_F64_PTR
        : field->kind == C_AGG_FIELD_PTR
            ? (field->agg ? (field->pointer_depth > 2 ? C_OPAQUE_PTR_PTR_PTR
                                                      : C_AGG_PTR_PTR)
               : field->is_function_pointer || field->elem_function_pointer
                   ? C_FUNC_PTR_PTR
               : field->elem_char_pointer || field->elem_pointer
                   ? C_OPAQUE_PTR_PTR
               : field->elem_size == 1 ? C_CHAR_PTR_PTR
                                       : C_OPAQUE_PTR_PTR)
            : C_BAD;
    if (pointer_type == C_BAD) {
      fail(p, "address-of this aggregate field type is unsupported");
      return (c_value_t){-1, C_BAD, NULL};
    }
    return (c_value_t){address, pointer_type, field->agg};
  }
  return base;
}

static c_value_t lower_pointer_index_rvalue(c_parser_t *p, c_value_t value) {
  while (c_type_is_pointer(value.type) && value.type != C_FUNC_PTR &&
         accept(p, "[")) {
    c_value_t index = parse_expr(p, 1);
    if (index.value < 0 || !c_type_is_integer(index.type) || !expect(p, "]")) {
      fail(p, "pointer index requires an integer");
      return (c_value_t){-1, C_BAD, NULL};
    }
    c_type_t element_type = c_remove_pointer_levels(value.type, 1);
    if (value.type == C_OPAQUE_PTR_PTR && value.agg)
      element_type = C_AGG_PTR;
    if (element_type == C_BAD) {
      fail(p, "pointer index requires a supported complete element type");
      return (c_value_t){-1, C_BAD, NULL};
    }
    int address = lower_pointer_index_address_for(p, value.type, value.agg,
                                                  value.value, index.value);
    if (element_type == C_AGG) {
      if (address < 0 || !record_lvalue(p, address, address, C_AGG,
                                        c_value_pointee_volatile(value)))
        return (c_value_t){-1, C_BAD, NULL};
      p->lvalues[p->lvalue_count - 1].agg = value.agg;
      value = (c_value_t){address, C_AGG, value.agg};
      continue;
    }
    unsigned flags =
        (element_type == C_U8 || element_type == C_CHAR ? NYIR_INST_F_MEM_BYTE
         : element_type == C_I32 || element_type == C_U32 ||
                 element_type == C_F32
             ? NYIR_INST_F_MEM_I32
             : 0) |
        c_value_seg_flags(value);
    int loaded =
        address < 0 ? -1
        : element_type == C_I16 || element_type == C_U16
            ? lower_load_u16_field_seg(p, address, element_type,
                                       c_value_pointee_volatile(value),
                                       c_value_seg_flags(value))
            : emit_value(
                  p, (nyr_inst_t){.op = NYIR_LOAD_I64,
                                  .dst = -1,
                                  .a = address,
                                  .effects = NYIR_EFFECT_READ_MEMORY |
                                             NYIR_EFFECT_MAY_TRAP |
                                             (c_value_pointee_volatile(value)
                                                  ? NYIR_EFFECT_VOLATILE
                                                  : 0),
                                  .flags = flags,
                                  .scalar_class =
                                      c_type_is_float(element_type)
                                          ? c_float_scalar_class(element_type)
                                          : NYIR_SCALAR_I64_RAW});
    if (loaded >= 0 && c_type_is_integer(element_type))
      loaded = normalize_integer(p, element_type, loaded);
    if (loaded < 0 || !record_lvalue(p, loaded, address, element_type,
                                     c_value_pointee_volatile(value)))
      return (c_value_t){-1, C_BAD, NULL};
    value = (c_value_t){loaded, element_type,
                        c_type_is_pointer(element_type) && value.agg
                            ? value.agg
                            : c_pointee_marker(c_value_pointee_const(value),
                                               c_value_pointee_volatile(value),
                                               c_marker_seg(value.agg))};
  }
  return value;
}

static int lower_array_shape_address(c_parser_t *p, int base,
                                     c_array_shape_t *shape) {
  if (!expect(p, "["))
    return -1;
  c_value_t index = parse_expr(p, 1);
  if (index.value < 0 || !c_type_is_integer(index.type) || !expect(p, "]") ||
      !shape->rank || !shape->element_size) {
    if (!p->err[0])
      fail(p, "multidimensional array index requires an integer");
    return -1;
  }
  size_t stride = shape->element_size;
  if (shape->pointer_to_array) {
    stride = c_array_shape_size(shape);
    if (!stride) {
      fail(p, "pointer-to-array stride overflows or has an incomplete extent");
      return -1;
    }
    shape->pointer_to_array = false;
  } else {
    for (unsigned dimension = 1; dimension < shape->rank; ++dimension) {
      if (shape->extent[dimension] &&
          stride > SIZE_MAX / shape->extent[dimension]) {
        fail(p, "multidimensional array row stride overflows");
        return -1;
      }
      stride *= shape->extent[dimension];
      shape->extent[dimension - 1] = shape->extent[dimension];
    }
    --shape->rank;
  }
  if (stride > INT64_MAX) {
    fail(p, "multidimensional array row stride exceeds the supported range");
    return -1;
  }
  return lower_pointer_offset_sized(p, (int64_t)stride, base, index.value,
                                    false);
}

static c_value_t lower_array_shape_index(c_parser_t *p, c_value_t value) {
  const c_array_shape_t *stored = array_shape_for_value(p, value.value);
  if (!stored || !tok_is(p, p->pos, "["))
    return value;
  c_array_shape_t shape = *stored;
  bool pointer_to_array = shape.pointer_to_array;
  int address = lower_array_shape_address(p, value.value, &shape);
  if (address < 0)
    return (c_value_t){-1, C_BAD, NULL};
  if (pointer_to_array) {
    if (!remember_array_shape(p, address, shape))
      return (c_value_t){-1, C_BAD, NULL};
    c_type_t pointer_type = c_array_pointer_type(shape.element_type);
    if (pointer_type == C_BAD) {
      fail(p, "pointer-to-array element type is unsupported");
      return (c_value_t){-1, C_BAD, NULL};
    }
    return (c_value_t){address, pointer_type,
                       c_value_pointee_volatile(value) ? value.agg : shape.agg};
  }
  if (shape.rank) {
    if (!remember_array_shape(p, address, shape))
      return (c_value_t){-1, C_BAD, NULL};
    return (c_value_t){address, value.type, value.agg};
  }
  if (shape.element_type == C_AGG)
    return (c_value_t){address, C_AGG, shape.agg};
  bool is_volatile = c_value_pointee_volatile(value);
  uint8_t flags = shape.element_type == C_U8 || shape.element_type == C_CHAR
                      ? NYIR_INST_F_MEM_BYTE
                  : shape.element_type == C_I32 ||
                          shape.element_type == C_U32 ||
                          shape.element_type == C_F32
                      ? NYIR_INST_F_MEM_I32
                      : 0;
  int loaded =
      shape.element_type == C_I16 || shape.element_type == C_U16
          ? lower_load_u16_field(p, address, shape.element_type, is_volatile)
          : emit_value(p, (nyr_inst_t){
                              .op = NYIR_LOAD_I64,
                              .dst = -1,
                              .a = address,
                              .effects = NYIR_EFFECT_READ_MEMORY |
                                         NYIR_EFFECT_MAY_TRAP,
                              .flags = flags,
                              .scalar_class =
                                  c_type_is_float(shape.element_type)
                                      ? c_float_scalar_class(shape.element_type)
                                      : NYIR_SCALAR_I64_RAW});
  if (loaded >= 0 && c_type_is_integer(shape.element_type))
    loaded = normalize_integer(p, shape.element_type, loaded);
  if (loaded < 0 ||
      !record_lvalue(p, loaded, address, shape.element_type, is_volatile))
    return (c_value_t){-1, C_BAD, NULL};
  return (c_value_t){loaded, shape.element_type,
                     c_type_is_pointer(shape.element_type) ? shape.agg : NULL};
}

static bool add_local(c_parser_t *p, ny_ctok_t name, c_type_t type, int value,
                      bool is_const, bool is_volatile);

static int lower_local_function_pointer_array(c_parser_t *p) {
  c_parser_t probe = *p;
  if (!tok_is(&probe, probe.pos, "(") || !tok_is(&probe, probe.pos + 1, "*") ||
      !tok_ident(probe.tokens[probe.pos + 2]) ||
      !tok_is(&probe, probe.pos + 3, "["))
    return 0;

  ++p->pos;
  if (!expect(p, "*"))
    return -1;
  ny_ctok_t name;
  if (!parse_name(p, &name) || !expect(p, "["))
    return -1;
  uint64_t extent = 0;
  if (!parse_array_extent_expr(p, 1, &extent) || !extent || extent > 65536 ||
      !expect(p, "]") || !expect(p, ")") || !expect(p, "(")) {
    fail(p, "local function-pointer array requires a fixed extent and type");
    return -1;
  }
  --p->pos;
  size_t params_after = skip_balanced(p, p->pos, "(", ")");
  if (params_after >= p->token_count) {
    fail(p, "unterminated local function-pointer array type");
    return -1;
  }
  p->pos = params_after;
  if (!expect(p, "=") || !expect(p, "{")) {
    fail(p, "local function-pointer arrays require an initializer");
    return -1;
  }
  if (extent > (uint64_t)INT32_MAX / sizeof(uint64_t)) {
    fail(p, "local function-pointer array exceeds frontend capacity");
    return -1;
  }
  int base =
      emit_value(p, (nyr_inst_t){.op = NYIR_ALLOCA,
                                 .dst = -1,
                                 .imm = (int64_t)(extent * sizeof(uint64_t)),
                                 .scalar_class = NYIR_SCALAR_I64_RAW});
  p->has_auto_arrays = true;
  if (base < 0 || !add_local(p, name, C_FUNC_PTR, base, false, false))
    return -1;
  c_local_t *local = &p->locals[p->local_count - 1];
  local->array_rank = 1;
  local->array_count = (size_t)extent;
  local->array_extent[0] = (size_t)extent;
  local->function_result_type = C_BAD;
  local->address_taken = true;

  size_t index = 0;
  while (!tok_is(p, p->pos, "}")) {
    if (index && !expect(p, ","))
      return -1;
    if (tok_is(p, p->pos, "}"))
      break;
    c_value_t function = parse_expr(p, 1);
    if (function.value < 0 || function.type != C_FUNC_PTR || index >= extent) {
      if (!p->err[0])
        fail(p, "local function-pointer array values must name functions");
      return -1;
    }
    int address = lower_add_const(p, base, (int64_t)(index * sizeof(uint64_t)));
    if (address < 0 || !emit(p,
                             (nyr_inst_t){.op = NYIR_STORE_I64,
                                          .dst = -1,
                                          .a = address,
                                          .c = function.value,
                                          .effects = NYIR_EFFECT_WRITE_MEMORY |
                                                     NYIR_EFFECT_MAY_TRAP,
                                          .scalar_class = NYIR_SCALAR_I64_RAW},
                             NULL))
      return -1;
    ++index;
  }
  if (!index || !expect(p, "}") || !expect(p, ";")) {
    if (!p->err[0])
      fail(p, "invalid local function-pointer array initializer");
    return -1;
  }
  return 1;
}

static bool lower_statement_range(c_parser_t *p, size_t start, size_t end,
                                  bool *returned);

static c_value_t parse_statement_expression_in_scope(c_parser_t *p, size_t open,
                                                     size_t after) {
  if (after == open + 2) {
    p->pos = after + 1;
    return (c_value_t){-1, C_BAD, NULL};
  }

  size_t final_semicolon = SIZE_MAX;
  size_t parens = 0;
  size_t brackets = 0;
  size_t braces = 0;
  for (size_t i = open + 1; i + 1 < after; ++i) {
    if (tok_is(p, i, "("))
      ++parens;
    else if (tok_is(p, i, ")") && parens)
      --parens;
    else if (tok_is(p, i, "["))
      ++brackets;
    else if (tok_is(p, i, "]") && brackets)
      --brackets;
    else if (tok_is(p, i, "{"))
      ++braces;
    else if (tok_is(p, i, "}") && braces)
      --braces;
    else if (tok_is(p, i, ";") && !parens && !brackets && !braces)
      final_semicolon = i;
  }
  /*
   * GNU permits a statement expression to end in a control statement, which
   * has no trailing semicolon of its own. In that case the last top-level
   * semicolon may belong to an earlier declaration; don't parse that
   * declaration as the expression value.
   */
  if (final_semicolon != SIZE_MAX) {
    size_t final_statement = final_semicolon + 1;
    if (final_statement < after - 1 &&
        (tok_is(p, final_statement, "if") ||
         tok_is(p, final_statement, "switch") ||
         tok_is(p, final_statement, "while") ||
         tok_is(p, final_statement, "do")) &&
        statement_end(p, final_statement) == after - 1) {
      size_t saved_local_count = p->local_count;
      size_t saved_scope_start = p->scope_start;
      size_t saved_typedef_count = p->typedef_count;
      p->scope_start = saved_local_count;
      bool returned = false;
      bool ok = lower_statement_range(p, open + 1, after - 1, &returned);
      p->local_count = saved_local_count;
      p->scope_start = saved_scope_start;
      p->typedef_count = saved_typedef_count;
      typedef_index_invalidate(p);
      if (!ok)
        return (c_value_t){-1, C_BAD, NULL};
      p->pos = after + 1;
      return (c_value_t){-1, C_BAD, NULL};
    }
  }
  if (final_semicolon == SIZE_MAX) {
    fail(p, "GNU statement expression must end with an expression statement");
    return (c_value_t){-1, C_BAD, NULL};
  }

  size_t saved_local_count = p->local_count;
  size_t saved_scope_start = p->scope_start;
  size_t saved_typedef_count = p->typedef_count;
  p->scope_start = p->local_count;
  bool returned = false;
  size_t body_start = open + 1;
  size_t preceding_end = body_start;
  while (preceding_end < final_semicolon) {
    size_t statement_after = statement_end(p, preceding_end);
    if (statement_after <= preceding_end ||
        statement_after > final_semicolon + 1) {
      fail(p, "malformed GNU statement expression body");
      p->typedef_count = saved_typedef_count;
      typedef_index_invalidate(p);
      p->local_count = saved_local_count;
      p->scope_start = saved_scope_start;
      return (c_value_t){-1, C_BAD, NULL};
    }
    if (statement_after == final_semicolon + 1 &&
        (tok_is(p, preceding_end, "{") || tok_is(p, preceding_end, "if") ||
         tok_is(p, preceding_end, "switch") ||
         tok_is(p, preceding_end, "while") || tok_is(p, preceding_end, "do") ||
         tok_is(p, preceding_end, "for") ||
         tok_is(p, preceding_end, "return") ||
         tok_is(p, preceding_end, "break") ||
         tok_is(p, preceding_end, "continue") ||
         tok_is(p, preceding_end, "goto"))) {
      preceding_end = statement_after;
      break;
    }
    if (statement_after == final_semicolon + 1)
      break;
    preceding_end = statement_after;
  }
  if (preceding_end > body_start &&
      !lower_statement_range(p, body_start, preceding_end, &returned)) {
    p->typedef_count = saved_typedef_count;
    typedef_index_invalidate(p);
    p->local_count = saved_local_count;
    p->scope_start = saved_scope_start;
    return (c_value_t){-1, C_BAD, NULL};
  }

  if (preceding_end == final_semicolon ||
      preceding_end == final_semicolon + 1) {
    p->pos = after + 1;
    p->typedef_count = saved_typedef_count;
    typedef_index_invalidate(p);
    p->local_count = saved_local_count;
    p->scope_start = saved_scope_start;
    return (c_value_t){-1, C_BAD, NULL};
  }

  if ((tok_is(p, preceding_end, "asm") || tok_is(p, preceding_end, "__asm") ||
       tok_is(p, preceding_end, "__asm__")) &&
      statement_end(p, preceding_end) == final_semicolon + 1) {
    if (!lower_statement_range(p, preceding_end, final_semicolon + 1,
                               &returned)) {
      p->typedef_count = saved_typedef_count;
      typedef_index_invalidate(p);
      p->local_count = saved_local_count;
      p->scope_start = saved_scope_start;
      return (c_value_t){-1, C_BAD, NULL};
    }
    p->pos = after + 1;
    p->typedef_count = saved_typedef_count;
    typedef_index_invalidate(p);
    p->local_count = saved_local_count;
    p->scope_start = saved_scope_start;
    return (c_value_t){-1, C_BAD, NULL};
  }

  p->pos = preceding_end;
  while (tok_ident(*current(p)) && tok_is(p, p->pos + 1, ":")) {
    ny_ctok_t name = *current(p);
    c_label_t *label = find_label_at(p, name, p->pos);
    if (!label || !emit_label(p, (int64_t)label->id)) {
      if (!p->err || !p->err_len || !p->err[0])
        fail(p, "could not resolve C label in GNU statement expression");
      p->typedef_count = saved_typedef_count;
      typedef_index_invalidate(p);
      p->local_count = saved_local_count;
      p->scope_start = saved_scope_start;
      return (c_value_t){-1, C_BAD, NULL};
    }
    p->pos += 2;
  }
  c_value_t result = parse_expr(p, 0);
  bool result_failed = result.value < 0 && p->err && p->err_len && p->err[0];
  while (p->pos < final_semicolon && tok_is(p, p->pos, ")"))
    ++p->pos;
  bool semicolon_consumed =
      p->pos == final_semicolon + 1 && tok_is(p, final_semicolon, ";");
  if (result_failed || (p->pos != final_semicolon && !semicolon_consumed) ||
      (p->pos == final_semicolon && !expect(p, ";"))) {
    if (!p->err || !p->err_len || !p->err[0]) {
      char message[192];
      ny_ctok_t token =
          p->pos < p->token_count ? p->tokens[p->pos] : (ny_ctok_t){0};
      snprintf(message, sizeof(message),
               "unsupported final statement in GNU statement expression "
               "(token `%.64s`, position %zu, expected %zu)",
               token.start ? token.start : "<end>", p->pos, final_semicolon);
      fail(p, message);
    }
    p->typedef_count = saved_typedef_count;
    typedef_index_invalidate(p);
    p->local_count = saved_local_count;
    p->scope_start = saved_scope_start;
    return (c_value_t){-1, C_BAD, NULL};
  }
  p->pos = after + 1;
  p->typedef_count = saved_typedef_count;
  typedef_index_invalidate(p);
  p->local_count = saved_local_count;
  p->scope_start = saved_scope_start;
  return result;
}

static c_value_t parse_statement_expression(c_parser_t *p) {
  size_t open = p->pos;
  size_t after = skip_balanced(p, open, "{", "}");
  if (after <= open + 1 || after >= p->token_count || !tok_is(p, after, ")")) {
    fail(p, "malformed GNU statement expression");
    return (c_value_t){-1, C_BAD, NULL};
  }
  c_cleanup_scope_t scope;
  enter_cleanup_scope(p, &scope, after - 1);
  c_value_t result = parse_statement_expression_in_scope(p, open, after);
  p->cleanup_scope = scope.parent;
  return result;
}

static bool generic_types_compatible(c_type_t left,
                                     const c_agg_type_t *left_agg,
                                     bool left_is_bool, bool left_pointee_const,
                                     c_type_t right, bool right_is_bool,
                                     bool right_pointee_const,
                                     const c_agg_type_t *right_agg) {
  if (left != right || left_is_bool != right_is_bool)
    return false;
  if (c_type_is_pointer(left) && left != C_FUNC_PTR &&
      left_pointee_const != right_pointee_const)
    return false;
  return (left != C_AGG && left != C_AGG_PTR && left != C_AGG_PTR_PTR) ||
         left_agg == right_agg;
}

static bool generic_long_long_associations_distinct(const c_parser_t *p,
                                                    size_t left_start,
                                                    size_t left_end,
                                                    size_t right_start,
                                                    size_t right_end) {
  bool left_char = false, right_char = false;
  bool left_signed_char = false, right_signed_char = false;
  bool left_unsigned = false, right_unsigned = false;
  bool left_pointer = false, right_pointer = false;
  for (size_t i = left_start; i < left_end; ++i) {
    left_char |= tok_is(p, i, "char");
    left_signed_char |= tok_is(p, i, "signed");
    left_unsigned |= tok_is(p, i, "unsigned");
    left_pointer |= tok_is(p, i, "*");
  }
  for (size_t i = right_start; i < right_end; ++i) {
    right_char |= tok_is(p, i, "char");
    right_signed_char |= tok_is(p, i, "signed");
    right_unsigned |= tok_is(p, i, "unsigned");
    right_pointer |= tok_is(p, i, "*");
  }
  if (left_char && right_char && (left_pointer == right_pointer) &&
      (left_signed_char != right_signed_char ||
       left_unsigned != right_unsigned))
    return true;
  size_t left_long = 0, right_long = 0;
  size_t left_pos = left_start, right_pos = right_start;
  while (left_pos < left_end || right_pos < right_end) {
    if (left_pos < left_end && tok_is(p, left_pos, "long")) {
      ++left_long;
      ++left_pos;
      continue;
    }
    if (right_pos < right_end && tok_is(p, right_pos, "long")) {
      ++right_long;
      ++right_pos;
      continue;
    }
    if (left_pos == left_end || right_pos == right_end)
      break;
    if (!tok_same(p->tokens[left_pos], p->tokens[right_pos]))
      return false;
    ++left_pos;
    ++right_pos;
  }
  return left_pos == left_end && right_pos == right_end &&
         ((left_long == 1 && right_long == 2) ||
          (left_long == 2 && right_long == 1));
}

static bool generic_control_matches_distinct_association(
    const c_parser_t *p, size_t control_start, size_t control_end,
    size_t association_start, size_t association_end, bool control_is_long_long,
    bool association_is_long_long) {
  size_t control_long_count = 0, association_long_count = 0;
  bool control_char = false, association_char = false;
  bool control_signed = false, association_signed = false;
  bool control_unsigned = false, association_unsigned = false;
  for (size_t i = control_start; i < control_end; ++i) {
    control_char |= tok_is(p, i, "char");
    control_signed |= tok_is(p, i, "signed");
    control_unsigned |= tok_is(p, i, "unsigned");
    control_long_count += tok_is(p, i, "long");
  }
  for (size_t i = association_start; i < association_end; ++i) {
    association_char |= tok_is(p, i, "char");
    association_signed |= tok_is(p, i, "signed");
    association_unsigned |= tok_is(p, i, "unsigned");
    association_long_count += tok_is(p, i, "long");
  }
  if (association_long_count)
    return (control_long_count ? control_long_count == 2
                               : control_is_long_long) ==
           (association_long_count == 2 || association_is_long_long);
  if (control_char && association_char &&
      (control_signed || control_unsigned || association_signed ||
       association_unsigned))
    return control_signed == association_signed &&
           control_unsigned == association_unsigned;
  return false;
}

static c_value_t parse_generic_selection(c_parser_t *p) {
  ++p->pos;
  if (!expect(p, "("))
    return (c_value_t){-1, C_BAD, NULL};
  size_t control_start = p->pos;
  c_parser_t control_probe = *p;
  nyr_func_t probe_out;
  expression_probe_begin(&control_probe, &probe_out);
  char probe_error[1] = {0};
  control_probe.err = probe_error;
  control_probe.err_len = sizeof(probe_error);
  control_probe.type_lookahead = true;
  c_value_t control = parse_expr(&control_probe, 1);
  size_t control_end = control_probe.pos;
  bool control_is_long_long = control_probe.last_type_long_long;
  bool control_is_bool = control_probe.last_type_bool;
  c_parser_t bool_probe = *p;
  char bool_probe_error[1] = {0};
  bool_probe.err = bool_probe_error;
  bool_probe.err_len = sizeof(bool_probe_error);
  bool_probe.type_lookahead = true;
  c_type_t control_type = C_BAD;
  if (static_typeof_expr_type(&bool_probe, &control_type, 0) &&
      control_type == control.type)
    control_is_bool = bool_probe.last_type_bool;
  bool control_pointee_const = control_probe.last_value_pointee_const;
  bool control_valid = control.value >= 0 && expect(&control_probe, ",");
  expression_probe_end(&control_probe);
  if (!control_valid) {
    fail(p, "_Generic requires a controlling expression and associations");
    return (c_value_t){-1, C_BAD, NULL};
  }
  p->pos = control_probe.pos;

  size_t selected_start = SIZE_MAX;
  size_t selected_end = SIZE_MAX;
  size_t selected_type_start = SIZE_MAX;
  size_t selected_type_end = SIZE_MAX;
  bool selected_type_is_long_long = false;
  size_t default_start = SIZE_MAX;
  size_t default_end = SIZE_MAX;
  c_type_t selected_function_result_type = C_BAD;
  c_agg_type_t *selected_function_result_agg = NULL;
  bool selected_function_void_params = false;
  c_type_t default_function_result_type = C_BAD;
  c_agg_type_t *default_function_result_agg = NULL;
  bool default_function_void_params = false;
  bool matched = false;
  for (;;) {
    bool is_default = accept(p, "default");
    c_type_t association_type = C_BAD;
    const c_agg_type_t *association_agg = NULL;
    bool association_is_bool = false;
    bool association_is_long_long = false;
    bool association_pointee_const = false;
    size_t association_type_start = p->pos;
    size_t association_type_end = p->pos;
    if (!is_default) {
      if (!parse_type(p, &association_type, true)) {
        fail(p, "_Generic association requires a type name or default");
        return (c_value_t){-1, C_BAD, NULL};
      }
      association_agg = p->last_type_agg;
      association_is_bool = p->last_type_bool;
      association_is_long_long = p->last_type_long_long;
      association_pointee_const =
          c_type_is_pointer(association_type) && p->last_type_const;
      association_type_end = p->pos;
    }
    if (!expect(p, ":")) {
      fail(p, "_Generic association requires ':'");
      return (c_value_t){-1, C_BAD, NULL};
    }
    size_t expression_start = p->pos;
    c_parser_t expression_probe = *p;
    expression_probe.err = probe_error;
    expression_probe.err_len = sizeof(probe_error);
    expression_probe.type_lookahead = true;
    c_type_t expression_type = C_BAD;
    bool selected_candidate =
        !is_default &&
        generic_types_compatible(control.type, control.agg, control_is_bool,
                                 control_pointee_const, association_type,
                                 association_is_bool, association_pointee_const,
                                 association_agg);
    bool association_valid;
    size_t expression_end;
    if (selected_candidate) {
      bool type_valid =
          static_typeof_expr_type(&expression_probe, &expression_type, 0);
      c_parser_t boundary_probe = *p;
      boundary_probe.type_lookahead = false;
      association_valid = skip_to_comma_or_close(&boundary_probe);
      expression_end = boundary_probe.pos;
      if (association_valid && expression_end < expression_start)
        expression_end += expression_start;
      if (!type_valid)
        expression_probe = boundary_probe;
    } else {
      expression_probe.type_lookahead = false;
      association_valid = skip_to_comma_or_close(&expression_probe);
      expression_end = expression_probe.pos;
    }
    c_type_t function_result_type =
        expression_probe.last_type_function_result_type;
    c_agg_type_t *function_result_agg =
        expression_probe.last_type_function_result_agg;
    bool function_void_params = expression_probe.last_type_function_void_params;
    if (!association_valid) {
      fail(p, "invalid _Generic association expression");
      return (c_value_t){-1, C_BAD, NULL};
    }
    if (is_default) {
      if (default_start != SIZE_MAX) {
        fail(p, "_Generic has more than one default association");
        return (c_value_t){-1, C_BAD, NULL};
      }
      default_start = expression_start;
      default_end = expression_end;
      default_function_result_type = function_result_type;
      default_function_result_agg = function_result_agg;
      default_function_void_params = function_void_params;
    } else if (selected_candidate) {
      bool distinct_association_types =
          matched && (generic_long_long_associations_distinct(
                          p, selected_type_start, selected_type_end,
                          association_type_start, association_type_end) ||
                      selected_type_is_long_long != association_is_long_long);
      if (matched && !distinct_association_types) {
        char message[256];
        ny_ctok_t first = p->tokens[selected_type_start];
        ny_ctok_t last = p->tokens[selected_type_end - 1];
        ny_ctok_t next = p->tokens[association_type_start];
        ny_ctok_t next_last = p->tokens[association_type_end - 1];
        size_t first_len = (size_t)(last.start + last.len - first.start);
        size_t next_len =
            (size_t)(next_last.start + next_last.len - next.start);
        snprintf(message, sizeof(message),
                 "_Generic has duplicate compatible associations `%.*s` and "
                 "`%.*s`",
                 (int)(first_len > 72 ? 72 : first_len), first.start,
                 (int)(next_len > 72 ? 72 : next_len), next.start);
        fail(p, message);
        return (c_value_t){-1, C_BAD, NULL};
      }
      if (matched && distinct_association_types &&
          generic_control_matches_distinct_association(
              p, control_start, control_end, association_type_start,
              association_type_end, control_is_long_long,
              association_is_long_long)) {
        selected_type_start = association_type_start;
        selected_type_end = association_type_end;
        selected_type_is_long_long = association_is_long_long;
        selected_start = expression_start;
        selected_end = expression_end;
        selected_function_result_type = function_result_type;
        selected_function_result_agg = function_result_agg;
        selected_function_void_params = function_void_params;
      }
      if (!matched) {
        matched = true;
        selected_type_start = association_type_start;
        selected_type_end = association_type_end;
        selected_type_is_long_long = association_is_long_long;
        selected_start = expression_start;
        selected_end = expression_end;
        selected_function_result_type = function_result_type;
        selected_function_result_agg = function_result_agg;
        selected_function_void_params = function_void_params;
      }
    }
    p->pos = expression_end;
    if (!accept(p, ","))
      break;
  }
  if (!expect(p, ")")) {
    fail(p, "unterminated _Generic selection");
    return (c_value_t){-1, C_BAD, NULL};
  }
  size_t selection_end = p->pos;
  if (!matched) {
    selected_start = default_start;
    selected_end = default_end;
    selected_function_result_type = default_function_result_type;
    selected_function_result_agg = default_function_result_agg;
    selected_function_void_params = default_function_void_params;
  }
  if (selected_start == SIZE_MAX) {
    fail(p, "_Generic has no matching association or default");
    return (c_value_t){-1, C_BAD, NULL};
  }
  p->pos = selected_start;
  c_value_t result = parse_expr(p, 1);
  bool void_result = result.value < 0 && result.type == C_BAD &&
                     (!p->err || !p->err_len || !p->err[0]);
  if ((!void_result && result.value < 0) || p->pos != selected_end) {
    char message[160];
    ny_ctok_t found =
        p->pos < p->token_count ? p->tokens[p->pos] : (ny_ctok_t){0};
    ny_ctok_t selected = p->tokens[selected_start];
    snprintf(message, sizeof(message),
             "failed to lower selected _Generic association at `%.40s` "
             "(next `%.40s`, position %zu/%zu)",
             selected.start ? selected.start : "<end>",
             found.start ? found.start : "<end>", p->pos, selected_end);
    fail(p, message);
    return (c_value_t){-1, C_BAD, NULL};
  }
  p->pos = selection_end;
  c_function_t *selected_function =
      selected_end == selected_start + 1 && tok_ident(p->tokens[selected_start])
          ? find_function(p, p->tokens[selected_start])
          : NULL;
  if (selected_function) {
    result.type = C_FUNC_PTR;
    result.agg = NULL;
    p->last_type_function_result_type = selected_function->result;
    p->last_type_function_result_agg = selected_function->result_agg;
    p->last_type_function_void_params =
        selected_function->param_count == 0 && !selected_function->variadic;
    p->indirect_bool_params = 0; /* set below only for sources that know it */
    p->indirect_result_type = selected_function->result;
    p->indirect_result_agg = selected_function->result_agg;
    p->has_indirect_result_signature = true;
  } else if (result.type == C_FUNC_PTR &&
             selected_function_result_type != C_BAD) {
    p->last_type_function_result_type = selected_function_result_type;
    p->last_type_function_result_agg = selected_function_result_agg;
    p->last_type_function_void_params = selected_function_void_params;
    p->indirect_bool_params = 0; /* set below only for sources that know it */
    p->indirect_result_type = selected_function_result_type;
    p->indirect_result_agg = selected_function_result_agg;
    p->has_indirect_result_signature = true;
  }
  return result;
}

#include "code/lang/c/parser/parts/builtins.h"

static c_value_t parse_primary(c_parser_t *p) {
  p->last_value_const = false;
  p->last_value_pointee_const = false;
  p->last_value_null_pointer_constant = false;
  p->last_value_array = false;
  p->has_indirect_result_signature = false;
  p->indirect_result_agg = NULL;
  ny_ctok_t tok = *current(p);
  if (tok_is(p, p->pos, "__real__") || tok_is(p, p->pos, "__imag__")) {
    bool imag_part = tok_is(p, p->pos, "__imag__");
    ++p->pos;
    if (accept(p, "*")) {
      c_value_t pointer = parse_unary(p);
      if (pointer.value < 0 ||
          (pointer.type != C_C64_PTR && pointer.type != C_C128_PTR)) {
        fail(p, "__real__/__imag__ requires a pointer to a double or float "
                "complex object");
        return (c_value_t){-1, C_BAD, NULL};
      }
      c_type_t pointed = pointer.type == C_C64_PTR ? C_C64 : C_C128;
      int base_address = pointer.value;
      if (accept(p, "[")) {
        c_value_t index = parse_expr(p, 1);
        if (index.value < 0 || !c_type_is_integer(index.type) ||
            !expect(p, "]")) {
          fail(p, "complex subscript requires an integer index");
          return (c_value_t){-1, C_BAD, NULL};
        }
        base_address = lower_pointer_index_address(p, pointer.type,
                                                   pointer.value, index.value);
        if (base_address < 0)
          return (c_value_t){-1, C_BAD, NULL};
      }
      int address =
          imag_part ? lower_add_const(p, base_address, pointed == C_C64 ? 4 : 8)
                    : base_address;
      if (address < 0)
        return (c_value_t){-1, C_BAD, NULL};
      int loaded = pointed == C_C64 ? lower_load_f32_field(p, address, false)
                                    : lower_load_field(p, address, true, false);
      c_type_t half_type = pointed == C_C64 ? C_F32 : C_F64;
      if (loaded < 0 || !record_lvalue(p, loaded, address, half_type, false))
        return (c_value_t){-1, C_BAD, NULL};
      return (c_value_t){loaded, half_type, NULL};
    }
    ny_ctok_t object = *current(p);
    c_local_t *part_local = find_local(p, object);
    c_global_t *part_global = part_local ? NULL : find_global(p, object);
    c_type_t part_type = part_local    ? part_local->type
                         : part_global ? part_global->type
                                       : C_BAD;
    if (!c_type_is_complex(part_type)) {
      /*
       * Not a plain complex object: parse a member or index operand through
       * the normal postfix lowering. Complex member rvalues arrive as an
       * address tagged with a synthetic aggregate whose scope_id is SIZE_MAX.
       */
      c_value_t whole = parse_unary(p);
      if (whole.value < 0 || whole.type != C_AGG || !whole.agg ||
          whole.agg->scope_id != SIZE_MAX) {
        fail(p, "__real__/__imag__ requires a double or float complex object");
        return (c_value_t){-1, C_BAD, NULL};
      }
      c_type_t whole_type = whole.agg->size == 8 ? C_C64 : C_C128;
      int whole_address =
          imag_part
              ? lower_add_const(p, whole.value, whole_type == C_C64 ? 4 : 8)
              : whole.value;
      if (whole_address < 0)
        return (c_value_t){-1, C_BAD, NULL};
      int whole_loaded = whole_type == C_C64
                             ? lower_load_f32_field(p, whole_address, false)
                             : lower_load_field(p, whole_address, true, false);
      c_type_t whole_half = whole_type == C_C64 ? C_F32 : C_F64;
      if (whole_loaded < 0 ||
          !record_lvalue(p, whole_loaded, whole_address, whole_half, false))
        return (c_value_t){-1, C_BAD, NULL};
      return (c_value_t){whole_loaded, whole_half, NULL};
    }
    if (part_type == C_C_LDOUBLE) {
      fail(p, "__real__/__imag__ requires a double or float complex object");
      return (c_value_t){-1, C_BAD, NULL};
    }
    ++p->pos;
    int base;
    if (part_local) {
      base = emit_value(p, (nyr_inst_t){.op = NYIR_ADDR_LOCAL,
                                        .dst = -1,
                                        .imm = (int64_t)part_local->slot,
                                        .scalar_class = NYIR_SCALAR_I64_RAW});
    } else {
      const char *prefix = (part_global->is_external || part_global->is_weak)
                               ? "__nyc_extern_"
                               : "__nyc_data_";
      size_t prefix_len = strlen(prefix), name_len = part_global->name.len;
      char *symbol = malloc(prefix_len + name_len + 1);
      if (!symbol) {
        fail(p, "out of memory lowering __real__/__imag__");
        return (c_value_t){-1, C_BAD, NULL};
      }
      memcpy(symbol, prefix, prefix_len);
      memcpy(symbol + prefix_len, part_global->name.start, name_len);
      symbol[prefix_len + name_len] = '\0';
      base = emit_value(p, (nyr_inst_t){.op = NYIR_ADDR_SYMBOL,
                                        .dst = -1,
                                        .symbol = symbol,
                                        .scalar_class = NYIR_SCALAR_I64_RAW});
      free(symbol);
    }
    if (base < 0)
      return (c_value_t){-1, C_BAD, NULL};
    int address =
        imag_part ? lower_add_const(p, base, part_type == C_C64 ? 4 : 8) : base;
    if (address < 0)
      return (c_value_t){-1, C_BAD, NULL};
    int loaded = part_type == C_C64 ? lower_load_f32_field(p, address, false)
                                    : lower_load_field(p, address, true, false);
    c_type_t half_type = part_type == C_C64 ? C_F32 : C_F64;
    if (loaded < 0 || !record_lvalue(p, loaded, address, half_type, false))
      return (c_value_t){-1, C_BAD, NULL};
    return (c_value_t){loaded, half_type, NULL};
  }
  if (tok_is(p, p->pos, "__builtin_choose_expr"))
    return parse_builtin_choose_expr(p);
  if (tok_is(p, p->pos, "__builtin_types_compatible_p"))
    return parse_builtin_types_compatible(p);
  if (tok_is(p, p->pos, "__builtin_counted_by_ref"))
    return parse_builtin_counted_by_ref(p);
  if (tok_is(p, p->pos, "_Generic"))
    return parse_generic_selection(p);
  if (accept(p, "(")) {
    if (tok_is(p, p->pos, "{"))
      return parse_statement_expression(p);
    c_parser_t type_probe = *p;
    char probe_error[256] = {0};
    type_probe.err = probe_error;
    type_probe.err_len = sizeof(probe_error);
    type_probe.type_lookahead = true;
    c_type_t cast_type;
    c_type_t function_pointer_result_type = C_BAD;
    c_agg_type_t *function_pointer_result_agg = NULL;
    bool is_cast = parse_type(&type_probe, &cast_type, true);
    bool array_compound_literal = false;
    c_type_t array_element_type = C_BAD;
    uint64_t array_compound_extent = 0;
    bool array_compound_has_extent = false;
    if (is_cast && accept(&type_probe, "[")) {
      array_compound_literal = true;
      array_element_type = cast_type;
      if (!tok_is(&type_probe, type_probe.pos, "]")) {
        if (!parse_array_extent_expr(&type_probe, 1, &array_compound_extent) ||
            !array_compound_extent || array_compound_extent > 65536) {
          is_cast = false;
        } else {
          array_compound_has_extent = true;
        }
      }
      if (!expect(&type_probe, "]"))
        is_cast = false;
      else if (is_cast)
        cast_type = c_add_pointer_levels(array_element_type, 1);
    }
    is_cast = is_cast && tok_is(&type_probe, type_probe.pos, ")");
    if (!is_cast)
      type_probe.pos = p->pos;
    if (!is_cast && (tok_is(&type_probe, type_probe.pos, "__typeof__") ||
                     tok_is(&type_probe, type_probe.pos, "typeof"))) {
      size_t typeof_open = type_probe.pos + 1;
      size_t typeof_end = skip_balanced(&type_probe, typeof_open, "(", ")");
      if (tok_is(&type_probe, typeof_open, "(") &&
          tok_is(&type_probe, typeof_end, ")")) {
        c_parser_t expression_probe = *p;
        expression_probe.pos = typeof_open + 1;
        expression_probe.token_count = typeof_end - 1;
        if (static_typeof_expr_type(&expression_probe, &cast_type, 0) &&
            expression_probe.pos == expression_probe.token_count) {
          type_probe = expression_probe;
          type_probe.token_count = p->token_count;
          type_probe.pos = typeof_end;
          is_cast = true;
        }
      }
    }
    if (!is_cast) {
      c_parser_t function_pointer_probe = *p;
      char function_pointer_error[1] = {0};
      function_pointer_probe.err = function_pointer_error;
      function_pointer_probe.err_len = sizeof(function_pointer_error);
      function_pointer_probe.type_lookahead = true;
      c_type_t result_type;
      if (parse_type(&function_pointer_probe, &result_type, true) &&
          accept(&function_pointer_probe, "(") &&
          accept(&function_pointer_probe, "*") &&
          accept(&function_pointer_probe, ")") &&
          accept(&function_pointer_probe, "(")) {
        size_t parameters_end = skip_balanced(
            &function_pointer_probe, function_pointer_probe.pos - 1, "(", ")");
        if (parameters_end > function_pointer_probe.pos &&
            tok_is(&function_pointer_probe, parameters_end, ")")) {
          function_pointer_probe.pos = parameters_end;
          type_probe = function_pointer_probe;
          cast_type = C_FUNC_PTR;
          function_pointer_result_type = result_type;
          is_cast = true;
        }
      }
    }
    if (!is_cast) {
      c_parser_t pointer_to_function_pointer_probe = *p;
      char pointer_to_function_pointer_error[1] = {0};
      pointer_to_function_pointer_probe.err = pointer_to_function_pointer_error;
      pointer_to_function_pointer_probe.err_len =
          sizeof(pointer_to_function_pointer_error);
      pointer_to_function_pointer_probe.type_lookahead = true;
      c_type_t result_type;
      if (parse_type(&pointer_to_function_pointer_probe, &result_type, true) &&
          accept(&pointer_to_function_pointer_probe, "(") &&
          accept(&pointer_to_function_pointer_probe, "*") &&
          accept(&pointer_to_function_pointer_probe, "*") &&
          accept(&pointer_to_function_pointer_probe, ")") &&
          accept(&pointer_to_function_pointer_probe, "(")) {
        size_t parameters_end =
            skip_balanced(&pointer_to_function_pointer_probe,
                          pointer_to_function_pointer_probe.pos - 1, "(", ")");
        if (parameters_end > pointer_to_function_pointer_probe.pos &&
            tok_is(&pointer_to_function_pointer_probe, parameters_end, ")")) {
          pointer_to_function_pointer_probe.pos = parameters_end;
          type_probe = pointer_to_function_pointer_probe;
          cast_type = C_FUNC_PTR_PTR;
          function_pointer_result_type = result_type;
          is_cast = true;
        }
      }
    }
    if (!is_cast) {
      c_parser_t nested_function_pointer_probe = *p;
      char nested_function_pointer_error[1] = {0};
      nested_function_pointer_probe.err = nested_function_pointer_error;
      nested_function_pointer_probe.err_len =
          sizeof(nested_function_pointer_error);
      nested_function_pointer_probe.type_lookahead = true;
      c_type_t result_type;
      if (parse_type(&nested_function_pointer_probe, &result_type, true) &&
          accept(&nested_function_pointer_probe, "(") &&
          accept(&nested_function_pointer_probe, "*") &&
          accept(&nested_function_pointer_probe, "(") &&
          accept(&nested_function_pointer_probe, "*") &&
          accept(&nested_function_pointer_probe, ")") &&
          accept(&nested_function_pointer_probe, "(")) {
        size_t params_end =
            skip_balanced(&nested_function_pointer_probe,
                          nested_function_pointer_probe.pos - 1, "(", ")");
        if (params_end < nested_function_pointer_probe.token_count &&
            tok_is(&nested_function_pointer_probe, params_end, ")") &&
            tok_is(&nested_function_pointer_probe, params_end + 1, "(")) {
          nested_function_pointer_probe.pos = params_end + 1;
          size_t result_params_end =
              skip_balanced(&nested_function_pointer_probe,
                            nested_function_pointer_probe.pos, "(", ")");
          if (result_params_end < nested_function_pointer_probe.token_count &&
              tok_is(&nested_function_pointer_probe, result_params_end, ")")) {
            nested_function_pointer_probe.pos = result_params_end;
            type_probe = nested_function_pointer_probe;
            cast_type = C_FUNC_PTR;
            function_pointer_result_type = C_FUNC_PTR;
            is_cast = true;
          }
        }
      }
    }
    if (is_cast) {
      if (type_probe.last_type_function_result_type != C_BAD) {
        function_pointer_result_type =
            type_probe.last_type_function_result_type;
        function_pointer_result_agg = type_probe.last_type_function_result_agg;
      }
      c_agg_type_t *cast_agg = complete_tag(p, type_probe.last_type_agg);
      bool cast_bool = cast_type == C_U8 && type_probe.last_type_bool;
      bool cast_pointee_const = type_probe.last_type_const;
      bool cast_pointee_volatile = type_probe.last_type_volatile;
      unsigned char cast_pointee_seg = type_probe.last_type_seg;
      bool cast_pointer_const = type_probe.last_type_const_pointer;
      c_array_shape_t cast_pointee_array = type_probe.last_type_pointee_array;
      p->pos = type_probe.pos + 1;
      if (array_compound_literal && tok_is(p, p->pos, "{")) {
        ++p->pos;
        size_t count = 0;
        size_t scan = p->pos;
        size_t braces = 0, parens = 0, brackets = 0;
        if (!tok_is(p, scan, "}")) {
          count = 1;
          for (; scan < p->token_count; ++scan) {
            if (tok_is(p, scan, "{"))
              ++braces;
            else if (tok_is(p, scan, "}")) {
              if (!braces && !parens && !brackets)
                break;
              if (braces)
                --braces;
            } else if (tok_is(p, scan, "("))
              ++parens;
            else if (tok_is(p, scan, ")") && parens)
              --parens;
            else if (tok_is(p, scan, "["))
              ++brackets;
            else if (tok_is(p, scan, "]") && brackets)
              --brackets;
            else if (tok_is(p, scan, ",") && !braces && !parens && !brackets &&
                     !tok_is(p, scan + 1, "}"))
              ++count;
          }
        }
        size_t width = array_element_type == C_AGG && cast_agg
                           ? cast_agg->size
                           : c_type_size(p->abi, array_element_type);
        c_value_t *values = NULL;
        size_t *indices = NULL;
        size_t parsed = 0;
        size_t element_count =
            array_compound_has_extent ? (size_t)array_compound_extent : count;
        if (!width || (!count && !array_compound_has_extent) ||
            scan >= p->token_count || !tok_is(p, scan, "}")) {
          fail(p, "invalid array compound literal");
          return (c_value_t){-1, C_BAD, NULL};
        }
        if (array_element_type == C_AGG) {
          if (count > element_count || element_count > 65536 ||
              element_count > SIZE_MAX / width ||
              element_count * width > INT64_MAX) {
            fail(p, "invalid array compound literal");
            return (c_value_t){-1, C_BAD, NULL};
          }
        } else {
          /*
           * GNU designated initializers supply explicit indices, so the
           * extent of an unsized compound literal is only known after
           * reading the list. `_PICK()` in the i915 register headers is this
           * exact shape.
           */
          values = count ? calloc(count, sizeof(*values)) : NULL;
          indices = count ? calloc(count, sizeof(*indices)) : NULL;
          if (count && (!values || !indices)) {
            free(values);
            free(indices);
            fail(p, "out of memory parsing array compound literal");
            return (c_value_t){-1, C_BAD, NULL};
          }
          parsed = 0;
          size_t next_index = 0;
          size_t highest = 0;
          bool have_designator = false;
          bool first_element = true;
          while (!tok_is(p, p->pos, "}")) {
            if (!first_element && !expect(p, ",")) {
              free(values);
              free(indices);
              return (c_value_t){-1, C_BAD, NULL};
            }
            if (tok_is(p, p->pos, "}"))
              break;
            size_t item_index = next_index;
            if (accept(p, "[")) {
              int64_t designator = -1;
              if (!parse_global_const_expr(p, &p->pos, &designator) ||
                  designator < 0 || tok_is(p, p->pos, "...") ||
                  !expect(p, "]") || !expect(p, "=")) {
                free(values);
                free(indices);
                fail(p, "invalid array compound literal designator");
                return (c_value_t){-1, C_BAD, NULL};
              }
              item_index = (size_t)designator;
              have_designator = true;
              if (item_index > highest)
                highest = item_index;
            }
            if (array_compound_has_extent &&
                item_index >= (size_t)array_compound_extent) {
              free(values);
              free(indices);
              fail(p, "array compound literal designator is out of range");
              return (c_value_t){-1, C_BAD, NULL};
            }
            c_value_t element = parse_expr(p, 1);
            bool compatible =
                element.type == array_element_type ||
                (c_type_is_integer(element.type) &&
                 c_type_is_integer(array_element_type)) ||
                c_pointer_compatible(element.type, array_element_type);
            if (element.value < 0 || !compatible) {
              free(values);
              free(indices);
              if (element.value >= 0)
                fail(p,
                     "scalar array compound literal has incompatible element");
              return (c_value_t){-1, C_BAD, NULL};
            }
            if (c_type_is_integer(array_element_type)) {
              element.value =
                  normalize_integer(p, array_element_type, element.value);
              if (element.value < 0) {
                free(values);
                free(indices);
                return (c_value_t){-1, C_BAD, NULL};
              }
            }
            values[parsed] = element;
            indices[parsed] = item_index;
            ++parsed;
            next_index = item_index + 1;
            first_element = false;
          }
          if (accept(p, ",") && !tok_is(p, p->pos, "}")) {
            free(values);
            free(indices);
            fail(p, "expected '}' after scalar array compound literal");
            return (c_value_t){-1, C_BAD, NULL};
          }
          if (!expect(p, "}")) {
            free(values);
            free(indices);
            return (c_value_t){-1, C_BAD, NULL};
          }
          element_count = array_compound_has_extent
                              ? (size_t)array_compound_extent
                          : have_designator ? highest + 1
                                            : parsed;
          if (!element_count || element_count > 65536 ||
              element_count > SIZE_MAX / width ||
              element_count * width > INT64_MAX) {
            free(values);
            free(indices);
            fail(p, "invalid array compound literal extent");
            return (c_value_t){-1, C_BAD, NULL};
          }
        }
        if (array_element_type == C_AGG) {
          if (!cast_agg || !cast_agg->complete) {
            fail(p,
                 "aggregate array compound literal requires a complete type");
            return (c_value_t){-1, C_BAD, NULL};
          }
          if (count != element_count) {
            fail(p, "aggregate array compound literal has omitted elements");
            return (c_value_t){-1, C_BAD, NULL};
          }
          size_t offset = 0;
          if (!agg_area_alloc(p, element_count * width, &offset))
            return (c_value_t){-1, C_BAD, NULL};
          int base = lower_add_const(p, p->agg_base, offset);
          if (base < 0 || !lower_zero_fill(p, base, element_count * width))
            return (c_value_t){-1, C_BAD, NULL};
          for (size_t i = 0; i < element_count; ++i) {
            if (!init_aggregate(p, NULL, cast_agg, NULL, i * width, 0, base))
              return (c_value_t){-1, C_BAD, NULL};
            if (i + 1 < element_count && !expect(p, ","))
              return (c_value_t){-1, C_BAD, NULL};
          }
          if ((accept(p, ",") && !tok_is(p, p->pos, "}")) || !expect(p, "}"))
            return (c_value_t){-1, C_BAD, NULL};
          return (c_value_t){base, cast_type, cast_agg};
        }
        int base =
            emit_value(p, (nyr_inst_t){.op = NYIR_ALLOCA,
                                       .dst = -1,
                                       .imm = (int64_t)(element_count * width),
                                       .scalar_class = NYIR_SCALAR_I64_RAW});
        if (base < 0)
          return (c_value_t){-1, C_BAD, NULL};
        uint8_t flags =
            array_element_type == C_U8 || array_element_type == C_CHAR
                ? NYIR_INST_F_MEM_BYTE
            : array_element_type == C_I32 || array_element_type == C_U32
                ? NYIR_INST_F_MEM_I32
                : 0;
        /*
         * Omitted and designated gaps are zero-initialized, so clear the
         * whole literal before storing the explicitly initialized elements.
         */
        for (size_t i = 0; i < element_count; ++i) {
          int zero =
              emit_value(p, (nyr_inst_t){.op = NYIR_CONST_I64,
                                         .dst = -1,
                                         .imm = 0,
                                         .scalar_class = NYIR_SCALAR_I64_RAW});
          int address =
              zero < 0 ? -1 : lower_add_const(p, base, (int64_t)(i * width));
          if (address < 0 ||
              !emit(p,
                    (nyr_inst_t){.op = NYIR_STORE_I64,
                                 .dst = -1,
                                 .a = address,
                                 .c = zero,
                                 .effects = NYIR_EFFECT_WRITE_MEMORY |
                                            NYIR_EFFECT_MAY_TRAP,
                                 .flags = flags,
                                 .scalar_class = NYIR_SCALAR_I64_RAW},
                    NULL)) {
            free(values);
            free(indices);
            return (c_value_t){-1, C_BAD, NULL};
          }
        }
        for (size_t i = 0; i < parsed; ++i) {
          int address = lower_add_const(p, base, (int64_t)(indices[i] * width));
          if (address < 0 ||
              !emit(p,
                    (nyr_inst_t){
                        .op = NYIR_STORE_I64,
                        .dst = -1,
                        .a = address,
                        .c = values[i].value,
                        .effects =
                            NYIR_EFFECT_WRITE_MEMORY | NYIR_EFFECT_MAY_TRAP,
                        .flags = flags,
                        .scalar_class =
                            c_type_is_float(array_element_type)
                                ? c_float_scalar_class(array_element_type)
                                : NYIR_SCALAR_I64_RAW},
                    NULL)) {
            free(values);
            free(indices);
            return (c_value_t){-1, C_BAD, NULL};
          }
        }
        free(values);
        free(indices);
        return (c_value_t){base, cast_type, NULL};
      }
      if (cast_type == C_AGG && tok_is(p, p->pos, "{")) {
        size_t offset = 0;
        if (!cast_agg || !cast_agg->complete ||
            !agg_area_alloc(p, cast_agg->size, &offset)) {
          if (!p->err[0])
            fail(p, "aggregate compound literal requires a complete type");
          return (c_value_t){-1, C_BAD, NULL};
        }
        int address = lower_add_const(p, p->agg_base, offset);
        if (address < 0 || !lower_zero_fill(p, address, cast_agg->size) ||
            !init_aggregate(p, NULL, cast_agg, NULL, 0, 0, address))
          return (c_value_t){-1, C_BAD, NULL};
        return (c_value_t){address, C_AGG, cast_agg};
      }
      size_t operand_start = p->pos;
      c_value_t operand = parse_unary(p);
      if (operand.value < 0)
        return operand;
      if (cast_bool &&
          (c_type_is_integer(operand.type) || c_type_is_pointer(operand.type) ||
           c_type_is_float(operand.type))) {
        int converted = lower_boolean_value(p, operand);
        return converted < 0 ? (c_value_t){-1, C_BAD, NULL}
                             : (c_value_t){converted, C_U8, NULL};
      }
      if (cast_pointee_array.rank && c_type_is_pointer(cast_type) &&
          !remember_array_shape(p, operand.value, cast_pointee_array))
        return (c_value_t){-1, C_BAD, NULL};
      if (cast_type == C_BAD) {
        const c_lvalue_t *lvalue = find_lvalue(p, operand.value);
        if (lvalue && !lvalue->is_volatile && discard_lvalue_read(p, lvalue))
          operand.value = emit_value(
              p, (nyr_inst_t){.op = NYIR_CONST_I64, .dst = -1, .imm = 0});
        p->last_value_null_pointer_constant = false;
        return (c_value_t){operand.value, C_BAD, NULL};
      }
      if (cast_type == C_AGG && cast_agg && cast_agg->is_int128 &&
          c_type_is_integer(operand.type)) {
        size_t offset = 0;
        if (!agg_area_alloc(p, cast_agg->size, &offset))
          return (c_value_t){-1, C_BAD, NULL};
        int low = normalize_integer(
            p,
            operand.type == C_I64 || operand.type == C_I32 ||
                    operand.type == C_I16 || operand.type == C_CHAR
                ? C_I64
                : C_U64,
            operand.value);
        if (low < 0)
          return (c_value_t){-1, C_BAD, NULL};
        int high = emit_i64_constant(p, 0);
        if (high < 0)
          return (c_value_t){-1, C_BAD, NULL};
        if (operand.type == C_I64 || operand.type == C_I32 ||
            operand.type == C_I16 || operand.type == C_CHAR) {
          int shift = emit_i64_constant(p, 63);
          high = shift < 0
                     ? -1
                     : emit_value(p, (nyr_inst_t){.op = NYIR_SAR_I64,
                                                  .dst = -1,
                                                  .a = low,
                                                  .b = shift,
                                                  .scalar_class =
                                                      NYIR_SCALAR_I64_RAW});
          if (high < 0)
            return (c_value_t){-1, C_BAD, NULL};
        }
        int low_address = lower_add_const(p, p->agg_base, offset);
        int high_address = lower_add_const(p, p->agg_base, offset + 8);
        if (low_address < 0 || high_address < 0 ||
            !emit(p,
                  (nyr_inst_t){.op = NYIR_STORE_I64,
                               .dst = -1,
                               .a = low_address,
                               .c = low,
                               .effects = NYIR_EFFECT_WRITE_MEMORY |
                                          NYIR_EFFECT_MAY_TRAP,
                               .scalar_class = NYIR_SCALAR_I64_RAW},
                  NULL) ||
            !emit(p,
                  (nyr_inst_t){.op = NYIR_STORE_I64,
                               .dst = -1,
                               .a = high_address,
                               .c = high,
                               .effects = NYIR_EFFECT_WRITE_MEMORY |
                                          NYIR_EFFECT_MAY_TRAP,
                               .scalar_class = NYIR_SCALAR_I64_RAW},
                  NULL))
          return (c_value_t){-1, C_BAD, NULL};
        int aggregate_address = lower_add_const(p, p->agg_base, offset);
        return aggregate_address < 0
                   ? (c_value_t){-1, C_BAD, NULL}
                   : (c_value_t){aggregate_address, C_AGG, cast_agg};
      }
      /*
       * GCC's scalar-to-union cast is used by the kernel to inspect a raw
       * register word through union bitfields.  Materialize the eight-byte
       * representation as aggregate storage so ordinary field lowering can
       * read the selected bits.
       */
      if (cast_type == C_AGG && cast_agg && cast_agg->complete &&
          cast_agg->is_union && cast_agg->size == 8 &&
          c_type_size(p->abi, operand.type) == 8 &&
          (c_type_is_integer(operand.type) ||
           c_type_is_pointer(operand.type))) {
        size_t offset = 0;
        if (!agg_area_alloc(p, cast_agg->size, &offset))
          return (c_value_t){-1, C_BAD, NULL};
        int address = lower_add_const(p, p->agg_base, offset);
        if (address < 0 ||
            !emit(p,
                  (nyr_inst_t){.op = NYIR_STORE_I64,
                               .dst = -1,
                               .a = address,
                               .c = operand.value,
                               .effects = NYIR_EFFECT_WRITE_MEMORY |
                                          NYIR_EFFECT_MAY_TRAP,
                               .scalar_class = NYIR_SCALAR_I64_RAW},
                  NULL))
          return (c_value_t){-1, C_BAD, NULL};
        return (c_value_t){address, C_AGG, cast_agg};
      }
      if (c_type_is_integer(cast_type) && operand.type == C_AGG &&
          operand.agg && operand.agg->is_int128) {
        int low_address = lower_add_const(p, operand.value, 0);
        if (low_address < 0)
          return (c_value_t){-1, C_BAD, NULL};
        int low =
            emit_value(p, (nyr_inst_t){.op = NYIR_LOAD_I64,
                                       .dst = -1,
                                       .a = low_address,
                                       .effects = NYIR_EFFECT_READ_MEMORY |
                                                  NYIR_EFFECT_MAY_TRAP,
                                       .scalar_class = NYIR_SCALAR_I64_RAW});
        if (low < 0)
          return (c_value_t){-1, C_BAD, NULL};
        int converted = normalize_integer(p, cast_type, low);
        return converted < 0 ? (c_value_t){-1, C_BAD, NULL}
                             : (c_value_t){converted, cast_type, NULL};
      }
      if ((cast_type == C_FUNC_PTR || cast_type == C_FUNC_PTR_PTR) &&
          function_pointer_result_type != C_BAD) {
        p->indirect_bool_params = 0; /* set below only for sources that know it */
        p->indirect_result_type = function_pointer_result_type;
        p->indirect_result_agg =
            function_pointer_result_type == C_AGG_PTR
                ? (function_pointer_result_agg ? function_pointer_result_agg
                                               : cast_agg)
                : NULL;
        p->has_indirect_result_signature = true;
      }
      if (cast_type == C_AGG_PTR_PTR && operand.type == C_AGG_PTR_PTR)
        return (c_value_t){operand.value, cast_type,
                           cast_agg ? cast_agg : operand.agg};
      if (cast_type == operand.type && !c_type_is_pointer(cast_type))
        return operand;
      if (cast_type == C_AGG_PTR && operand.type == C_AGG_PTR)
        return (c_value_t){operand.value, cast_type,
                           cast_agg ? cast_agg : operand.agg};
      if (c_type_is_integer(cast_type) &&
          (c_type_is_pointer(operand.type) || operand.type == C_AGG)) {
        int converted = normalize_integer(p, cast_type, operand.value);
        return converted < 0 ? (c_value_t){-1, C_BAD, NULL}
                             : (c_value_t){converted, cast_type, NULL};
      }
      if (c_type_is_integer(cast_type) && c_type_is_integer(operand.type)) {
        int converted = normalize_integer(p, cast_type, operand.value);
        return converted < 0 ? (c_value_t){-1, C_BAD, NULL}
                             : (c_value_t){converted, cast_type, NULL};
      }
      if (c_type_is_integer(cast_type) && c_type_is_float(operand.type)) {
        if (cast_type == C_U64) {
          const char *helper =
              operand.type == C_F32 ? "__fixunssfdi" : "__fixunsdfdi";
          nyr_inst_t call = {.op = NYIR_CALL,
                             .dst = -1,
                             .a = operand.value,
                             .imm = 1,
                             .flags = NYIR_INST_F_EXTERN,
                             .effects = NYIR_EFFECT_CALL,
                             .scalar_class = NYIR_SCALAR_I64_RAW,
                             .symbol = helper};
          nyr_call_arg_class_set(call.arg_scalar_classes, 0,
                                 c_float_scalar_class(operand.type));
          int converted = emit_value(p, call);
          return converted < 0 ? (c_value_t){-1, C_BAD, NULL}
                               : (c_value_t){converted, C_U64, NULL};
        }
        int converted = emit_value(
            p, (nyr_inst_t){.op = operand.type == C_F32 ? NYIR_F32_TO_I64
                                                        : NYIR_F64_TO_I64,
                            .dst = -1,
                            .a = operand.value,
                            .scalar_class = NYIR_SCALAR_I64_RAW});
        if (converted < 0)
          return (c_value_t){-1, C_BAD, NULL};
        converted = normalize_integer(p, cast_type, converted);
        return converted < 0 ? (c_value_t){-1, C_BAD, NULL}
                             : (c_value_t){converted, cast_type, NULL};
      }
      if (c_type_is_float(cast_type) && c_type_is_integer(operand.type)) {
        if (operand.type == C_U64) {
          nyr_inst_t call = {.op = NYIR_CALL,
                             .dst = -1,
                             .a = operand.value,
                             .imm = 1,
                             .flags = NYIR_INST_F_EXTERN,
                             .effects = NYIR_EFFECT_CALL,
                             .scalar_class = c_float_scalar_class(cast_type),
                             .symbol = cast_type == C_F32 ? "__floatundisf"
                                                          : "__floatundidf"};
          nyr_call_arg_class_set(call.arg_scalar_classes, 0,
                                 NYIR_SCALAR_I64_RAW);
          int converted = emit_value(p, call);
          return converted < 0 ? (c_value_t){-1, C_BAD, NULL}
                               : (c_value_t){converted, cast_type, NULL};
        }
        int converted = emit_value(
            p, (nyr_inst_t){.op = cast_type == C_F32 ? NYIR_I64_TO_F32
                                                     : NYIR_I64_TO_F64,
                            .dst = -1,
                            .a = operand.value,
                            .scalar_class = c_float_scalar_class(cast_type)});
        return converted < 0 ? (c_value_t){-1, C_BAD, NULL}
                             : (c_value_t){converted, cast_type, NULL};
      }
      if (c_type_is_float(cast_type) && c_type_is_float(operand.type)) {
        int converted = emit_value(
            p, (nyr_inst_t){.op = cast_type == C_F32 ? NYIR_F64_TO_F32
                                                     : NYIR_F32_TO_F64,
                            .dst = -1,
                            .a = operand.value,
                            .scalar_class = c_float_scalar_class(cast_type)});
        return converted < 0 ? (c_value_t){-1, C_BAD, NULL}
                             : (c_value_t){converted, cast_type, NULL};
      }
      if (cast_type == C_CHAR_PTR && c_type_is_pointer(operand.type)) {
        p->last_value_const = cast_pointer_const;
        p->last_value_pointee_const = cast_pointee_const;
        p->last_value_pointee_seg = cast_pointee_seg;
        const c_agg_type_t *pointee_marker = c_pointee_marker(
            cast_pointee_const, cast_pointee_volatile, cast_pointee_seg);
        return (c_value_t){operand.value, C_CHAR_PTR, pointee_marker};
      }
      if (c_type_is_pointer(cast_type) && c_type_is_pointer(operand.type)) {
        if (cast_type == C_FUNC_PTR && function_pointer_result_type != C_BAD) {
          p->indirect_bool_params = 0; /* set below only for sources that know it */
          p->indirect_result_type = function_pointer_result_type;
          p->indirect_result_agg =
              function_pointer_result_type == C_AGG_PTR ? cast_agg : NULL;
          p->has_indirect_result_signature = true;
        }
        p->last_value_const = cast_pointer_const;
        p->last_value_pointee_const = cast_pointee_const;
        p->last_value_pointee_seg = cast_pointee_seg;
        const c_agg_type_t *result_agg = cast_agg ? cast_agg : operand.agg;
        if (result_agg == &c_const_pointee_marker ||
            result_agg == &c_volatile_pointee_marker ||
            result_agg == &c_const_volatile_pointee_marker)
          result_agg = NULL;
        if (!result_agg && cast_type != C_FUNC_PTR &&
            cast_type != C_FUNC_PTR_PTR && cast_type != C_AGG_PTR &&
            cast_type != C_AGG_PTR_PTR)
          result_agg = c_pointee_marker(
              cast_pointee_const, cast_pointee_volatile, cast_pointee_seg);
        return (c_value_t){operand.value, cast_type, result_agg};
      }
      if (c_type_is_pointer(cast_type) && operand.type == C_AGG) {
        p->last_value_const = cast_pointer_const;
        p->last_value_pointee_const = cast_pointee_const;
        p->last_value_pointee_seg = cast_pointee_seg;
        return (c_value_t){operand.value, cast_type, cast_agg};
      }
      if (cast_type == C_AGG && cast_agg && c_type_is_integer(operand.type)) {
        p->last_value_const = cast_pointer_const;
        p->last_value_pointee_const = cast_pointee_const;
        p->last_value_pointee_seg = cast_pointee_seg;
        return (c_value_t){operand.value, C_AGG_PTR, cast_agg};
      }
      if (c_type_is_pointer(cast_type) && c_type_is_integer(operand.type)) {
        c_parser_t constant_probe = *p;
        char constant_error[1] = {0};
        constant_probe.err = constant_error;
        constant_probe.err_len = sizeof(constant_error);
        size_t constant_end = operand_start;
        int64_t integer_constant = 0;
        p->last_value_null_pointer_constant =
            parse_global_const_expr(&constant_probe, &constant_end,
                                    &integer_constant) &&
            constant_end == p->pos && integer_constant == 0;
        p->last_value_const = cast_pointer_const;
        p->last_value_pointee_const = cast_pointee_const;
        p->last_value_pointee_seg = cast_pointee_seg;
        const c_agg_type_t *result_agg = cast_agg;
        if (!result_agg && c_type_is_pointer(cast_type) &&
            cast_type != C_FUNC_PTR && cast_type != C_FUNC_PTR_PTR &&
            cast_type != C_AGG_PTR && cast_type != C_AGG_PTR_PTR &&
            cast_type != C_AGG_ARRAY_PTR)
          result_agg = c_pointee_marker(
              cast_pointee_const, cast_pointee_volatile, cast_pointee_seg);
        return (c_value_t){operand.value, cast_type, result_agg};
      }
      if (c_type_is_pointer(cast_type) && p->last_value_null_pointer_constant)
        return (c_value_t){operand.value, cast_type, NULL};
      char message[128];
      snprintf(message, sizeof(message),
               "unsupported cast or implicit conversion (from type %d to %d)",
               (int)operand.type, (int)cast_type);
      fail(p, message);
      return (c_value_t){-1, C_BAD, NULL};
    }
    c_value_t value = parse_expr(p, 0);
    (void)expect(p, ")");
    if (value.value >= 0 && value.type == C_FUNC_PTR && accept(p, "("))
      return parse_indirect_call(p, value);
    while (value.value >= 0 && array_shape_for_value(p, value.value) &&
           tok_is(p, p->pos, "[")) {
      value = lower_array_shape_index(p, value);
      if (value.value < 0)
        return value;
    }
    if (value.value >= 0 && c_type_is_pointer(value.type) &&
        value.type != C_FUNC_PTR) {
      while (accept(p, "[")) {
        c_value_t index = parse_expr(p, 1);
        if (index.value < 0 || !c_type_is_integer(index.type) ||
            !expect(p, "]")) {
          fail(p, "pointer subscript requires an integer index");
          return (c_value_t){-1, C_BAD, NULL};
        }
        bool aggregate_element =
            value.type == C_AGG_PTR && value.agg && value.agg->complete;
        int address =
            aggregate_element
                ? lower_pointer_index_address_for(p, value.type, value.agg,
                                                  value.value, index.value)
                : lower_pointer_index_address(p, value.type, value.value,
                                              index.value);
        if (address < 0)
          return (c_value_t){-1, C_BAD, NULL};
        if (aggregate_element) {
          if (!record_lvalue(p, address, address, C_AGG, false))
            return (c_value_t){-1, C_BAD, NULL};
          p->lvalues[p->lvalue_count - 1].agg = value.agg;
          value = (c_value_t){address, C_AGG, value.agg};
          break;
        }
        c_type_t element_type;
        unsigned flags = 0;
        if (value.type == C_CHAR_PTR || value.type == C_I8_PTR) {
          element_type = value.type == C_I8_PTR ? C_CHAR : C_U8;
          flags = NYIR_INST_F_MEM_BYTE;
        } else if (value.type == C_I32_PTR || value.type == C_U32_PTR) {
          element_type = value.type == C_U32_PTR ? C_U32 : C_I32;
          flags = NYIR_INST_F_MEM_I32;
        } else if (value.type == C_I16_PTR || value.type == C_U16_PTR) {
          element_type = value.type == C_U16_PTR ? C_U16 : C_I16;
        } else if (value.type == C_F32_PTR) {
          element_type = C_F32;
        } else if (value.type == C_F64_PTR) {
          element_type = C_F64;
        } else if (value.type == C_I64_PTR || value.type == C_U64_PTR) {
          element_type = value.type == C_U64_PTR ? C_U64 : C_I64;
        } else {
          element_type = value.type == C_FUNC_PTR_PTR   ? C_FUNC_PTR
                         : value.type == C_CHAR_PTR_PTR ? C_CHAR_PTR
                                                        : C_OPAQUE_PTR;
        }
        int loaded = emit_value(
            p, (nyr_inst_t){
                   .op = NYIR_LOAD_I64,
                   .dst = -1,
                   .a = address,
                   .effects = NYIR_EFFECT_READ_MEMORY | NYIR_EFFECT_MAY_TRAP,
                   .flags = flags,
                   .scalar_class = c_type_is_float(element_type)
                                       ? c_float_scalar_class(element_type)
                                       : NYIR_SCALAR_I64_RAW});
        if (loaded >= 0 && c_type_is_integer(element_type))
          loaded = normalize_integer(p, element_type, loaded);
        if (loaded < 0 ||
            !record_lvalue(p, loaded, address, element_type, false))
          return (c_value_t){-1, C_BAD, NULL};
        value = (c_value_t){loaded, element_type,
                            element_type == C_AGG_PTR ? value.agg : NULL};
      }
    }
    if (value.value >= 0 && (value.type == C_AGG || value.type == C_AGG_PTR)) {
      c_value_t member;
      if (!lower_member_rvalue(p, value, &member))
        return (c_value_t){-1, C_BAD, NULL};
      value = member;
    }
    return value;
  }
  if (tok.kind == NY_CTOK_STRING) {
    c_value_t string = lower_string_literal(p);
    return string.value < 0 ? string : lower_pointer_index_rvalue(p, string);
  }
  if (tok.kind == NY_CTOK_CHAR) {
    uint64_t value = 0;
    if (!parse_global_byte_value(tok, &value)) {
      fail(p, "unsupported character constant");
      return (c_value_t){-1, C_BAD, NULL};
    }
    ++p->pos;
    return (c_value_t){emit_value(p, (nyr_inst_t){.op = NYIR_CONST_I64,
                                                  .dst = -1,
                                                  .imm = (int64_t)value}),
                       C_I32, NULL};
  }
  if (tok.kind == NY_CTOK_NUMBER) {
    ++p->pos;
    char buffer[128];
    if (tok.len >= sizeof(buffer)) {
      fail(p, "numeric literal exceeds frontend limit");
      return (c_value_t){-1, C_BAD, NULL};
    }
    memcpy(buffer, tok.start, tok.len);
    buffer[tok.len] = '\0';
    size_t length = tok.len;
    bool imaginary_literal = false;
    if (length > 1 &&
        (buffer[length - 1] == 'i' || buffer[length - 1] == 'I')) {
      imaginary_literal = true;
      buffer[--length] = '\0';
    } else if (length > 2 &&
               (buffer[length - 2] == 'i' || buffer[length - 2] == 'I') &&
               (buffer[length - 1] == 'f' || buffer[length - 1] == 'F' ||
                buffer[length - 1] == 'l' || buffer[length - 1] == 'L')) {
      /*
       * System complex.h spells _Complex_I as `1.0iF`: the imaginary marker
       * precedes the precision suffix.
       */
      imaginary_literal = true;
      buffer[length - 2] = buffer[length - 1];
      buffer[--length] = '\0';
    }
    bool hexadecimal =
        buffer[0] == '0' && (buffer[1] == 'x' || buffer[1] == 'X');
    bool floating = strchr(buffer, '.') != NULL ||
                    (hexadecimal ? strpbrk(buffer, "pP") != NULL
                                 : strpbrk(buffer, "eE") != NULL);
    bool f32_literal =
        (buffer[length - 1] == 'f' || buffer[length - 1] == 'F') &&
        (!hexadecimal || strpbrk(buffer, "pP") != NULL);
    bool long_double_literal =
        floating && (buffer[length - 1] == 'l' || buffer[length - 1] == 'L');
    if (f32_literal || long_double_literal)
      buffer[length - 1] = '\0';
    if (floating) {
      char *end = NULL;
      double number =
          f32_literal ? (double)strtof(buffer, &end) : strtod(buffer, &end);
      if (!end || end == buffer || *end) {
        fail(p, "invalid double literal");
        return (c_value_t){-1, C_BAD, NULL};
      }
      if (imaginary_literal) {
        static c_agg_type_t imaginary_f32 = {
            .scope_id = SIZE_MAX,
            .complete = true,
            .size = 8,
            .align = 4,
            .field_count = 2,
            .fields = {
                {.kind = C_AGG_FIELD_F32, .offset = 0, .size = 4, .align = 4},
                {.kind = C_AGG_FIELD_F32, .offset = 4, .size = 4, .align = 4}}};
        static c_agg_type_t imaginary_f64 = {
            .scope_id = SIZE_MAX,
            .complete = true,
            .size = 16,
            .align = 8,
            .field_count = 2,
            .fields = {
                {.kind = C_AGG_FIELD_F64, .offset = 0, .size = 8, .align = 8},
                {.kind = C_AGG_FIELD_F64, .offset = 8, .size = 8, .align = 8}}};
        size_t offset = 0;
        if (!agg_area_alloc(p, f32_literal ? 8 : 16, &offset)) {
          fail(p, "imaginary literal storage allocation failed");
          return (c_value_t){-1, C_BAD, NULL};
        }
        int base = p->agg_base;
        int zero = emit_value(
            p, (nyr_inst_t){.op = f32_literal ? NYIR_CONST_F32 : NYIR_CONST_F64,
                            .dst = -1,
                            .imm = 0,
                            .scalar_class = f32_literal ? NYIR_SCALAR_F32
                                                        : NYIR_SCALAR_F64});
        int component;
        if (f32_literal) {
          float narrowed = (float)number;
          uint32_t bits = 0;
          memcpy(&bits, &narrowed, sizeof(bits));
          component =
              emit_value(p, (nyr_inst_t){.op = NYIR_CONST_F32,
                                         .dst = -1,
                                         .imm = (int64_t)bits,
                                         .scalar_class = NYIR_SCALAR_F32});
        } else {
          int64_t bits = 0;
          memcpy(&bits, &number, sizeof(bits));
          component =
              emit_value(p, (nyr_inst_t){.op = NYIR_CONST_F64,
                                         .dst = -1,
                                         .imm = bits,
                                         .scalar_class = NYIR_SCALAR_F64});
        }
        int high = lower_add_const(p, base, f32_literal ? 4 : 8);
        bool stored =
            zero >= 0 && component >= 0 && high >= 0 &&
            (f32_literal
                 ? lower_store_f32_field(p, base, zero, false) &&
                       lower_store_f32_field(p, high, component, false)
                 : lower_store_field(p, base, zero, true, false) &&
                       lower_store_field(p, high, component, true, false));
        if (!stored)
          return (c_value_t){-1, C_BAD, NULL};
        return (c_value_t){base, C_AGG,
                           f32_literal ? &imaginary_f32 : &imaginary_f64};
      }
      if (f32_literal) {
        float narrowed = (float)number;
        uint32_t bits = 0;
        memcpy(&bits, &narrowed, sizeof(bits));
        return (c_value_t){
            emit_value(p, (nyr_inst_t){.op = NYIR_CONST_F32,
                                       .dst = -1,
                                       .imm = (int64_t)bits,
                                       .scalar_class = NYIR_SCALAR_F32}),
            C_F32, NULL};
      }
      int64_t bits = 0;
      memcpy(&bits, &number, sizeof(bits));
      return (c_value_t){
          emit_value(p, (nyr_inst_t){.op = NYIR_CONST_F64,
                                     .dst = -1,
                                     .imm = bits,
                                     .scalar_class = NYIR_SCALAR_F64}),
          C_F64, NULL};
    }
    size_t end = length;
    bool unsigned_suffix = false;
    unsigned long_suffixes = 0;
    for (size_t i = end; i > 0; --i) {
      char suffix = buffer[i - 1];
      if (suffix == 'u' || suffix == 'U')
        unsigned_suffix = true;
      else if (suffix == 'l' || suffix == 'L')
        ++long_suffixes;
      else
        break;
    }
    while (end && (buffer[end - 1] == 'u' || buffer[end - 1] == 'U' ||
                   buffer[end - 1] == 'l' || buffer[end - 1] == 'L'))
      buffer[--end] = '\0';
    char *endptr = NULL;
    errno = 0;
    long long number = strtoll(buffer, &endptr, 0);
    c_type_t number_type = C_I64;
    int64_t number_bits = (int64_t)number;
    if (errno == ERANGE && endptr && endptr != buffer && !*endptr) {
      errno = 0;
      unsigned long long unsigned_number = strtoull(buffer, &endptr, 0);
      if (errno == ERANGE || !endptr || endptr == buffer || *endptr) {
        fail(p, "integer literal exceeds the supported 64-bit range");
        return (c_value_t){-1, C_BAD, NULL};
      }
      memcpy(&number_bits, &unsigned_number, sizeof(number_bits));
      number_type = C_U64;
    } else if (unsigned_suffix) {
      if (!long_suffixes || (long_suffixes == 1 && p->abi == NYC_ABI_I386)) {
        if (number < 0 || (unsigned long long)number > UINT32_MAX) {
          number_type = C_U64;
        } else {
          number_type = C_U32;
        }
      } else {
        number_type = C_U64;
      }
      if (number_type == C_U64) {
        unsigned long long unsigned_number =
            (unsigned long long)strtoull(buffer, NULL, 0);
        memcpy(&number_bits, &unsigned_number, sizeof(number_bits));
      }
    } else if (!endptr || endptr == buffer || *endptr) {
      char message[160];
      snprintf(message, sizeof(message), "invalid integer literal `%.*s`",
               (int)tok.len, tok.start);
      fail(p, message);
      return (c_value_t){-1, C_BAD, NULL};
    }
    if (number_type == C_I64 &&
        (!long_suffixes || (long_suffixes == 1 && p->abi == NYC_ABI_I386))) {
      if (number <= INT32_MAX)
        number_type = C_I32;
      else if (buffer[0] == '0' && (unsigned long long)number <= UINT32_MAX)
        number_type = C_U32;
    }
    p->last_value_null_pointer_constant = number_bits == 0;
    return (c_value_t){emit_value(p, (nyr_inst_t){.op = NYIR_CONST_I64,
                                                  .dst = -1,
                                                  .imm = number_bits}),
                       number_type, NULL};
  }
  if (tok_ident(tok) &&
      (tok_is(p, p->pos, "asm") || tok_is(p, p->pos, "__asm") ||
       tok_is(p, p->pos, "__asm__"))) {
    fail(p, "GNU inline assembly is not supported by NYC");
    return (c_value_t){-1, C_BAD, NULL};
  }
  if (tok_ident(tok)) {
    const c_enum_constant_t *enumerator =
        find_local(p, tok) ? NULL : find_enum_constant(p, tok);
    if (enumerator) {
      ++p->pos;
      p->last_value_null_pointer_constant = enumerator->value == 0;
      return (c_value_t){emit_value(p, (nyr_inst_t){.op = NYIR_CONST_I64,
                                                    .dst = -1,
                                                    .imm = enumerator->value}),
                         enumerator->type, NULL};
    }
    c_local_t *callee = find_local(p, tok);
    if (callee && callee->type == C_FUNC_PTR && tok_is(p, p->pos + 1, "(")) {
      ++p->pos;
      ++p->pos;
      int target = lower_local_load(p, callee);
      p->indirect_bool_params = 0; /* set below only for sources that know it */
      p->indirect_result_type = callee->function_result_type;
      p->indirect_result_agg = callee->agg;
      p->has_indirect_result_signature = true;
      p->indirect_bool_params = callee->function_bool_params;
      return parse_indirect_call(p, (c_value_t){target, C_FUNC_PTR, NULL});
    }
    if (tok_same(tok, (ny_ctok_t){.start = "NULL", .len = 4}) && !callee &&
        !find_global(p, tok)) {
      ++p->pos;
      p->last_value_null_pointer_constant = true;
      return (c_value_t){
          emit_value(p,
                     (nyr_inst_t){.op = NYIR_CONST_I64, .dst = -1, .imm = 0}),
          C_I32, NULL};
    }
    if (tok_same(tok, (ny_ctok_t){.start = "__func__",
                                  .len = sizeof("__func__") - 1}) &&
        p->function) {
      size_t token_pos = p->pos;
      ny_ctok_t original = p->tokens[token_pos];
      size_t quoted_len = p->function->name.len + 2;
      char *quoted = malloc(quoted_len + 1);
      if (!quoted) {
        fail(p, "out of memory lowering __func__");
        return (c_value_t){-1, C_BAD, NULL};
      }
      quoted[0] = '"';
      memcpy(quoted + 1, p->function->name.start, p->function->name.len);
      quoted[quoted_len - 1] = '"';
      quoted[quoted_len] = '\0';
      p->tokens[token_pos] = (ny_ctok_t){
          .kind = NY_CTOK_STRING, .start = quoted, .len = quoted_len};
      c_value_t function_name = lower_string_literal(p);
      p->tokens[token_pos] = original;
      free(quoted);
      return function_name.value < 0
                 ? function_name
                 : lower_pointer_index_rvalue(p, function_name);
    }
    ++p->pos;
    if (accept(p, "(")) {
      if (tok_same(tok, (ny_ctok_t){.start = "__builtin_expect",
                                    .len = sizeof("__builtin_expect") - 1})) {
        c_value_t expected = parse_expr(p, 1);
        bool null_pointer_constant = p->last_value_null_pointer_constant;
        if (expected.value < 0 || !expect(p, ","))
          return (c_value_t){-1, C_BAD, NULL};
        c_value_t probability = parse_expr(p, 1);
        if (probability.value < 0 || !expect(p, ")") ||
            (!c_type_is_integer(expected.type) &&
             !c_type_is_pointer(expected.type)) ||
            (!c_type_is_integer(probability.type) &&
             !c_type_is_pointer(probability.type))) {
          if (!p->err || !p->err_len || !p->err[0])
            fail(p, "__builtin_expect requires scalar arguments");
          return (c_value_t){-1, C_BAD, NULL};
        }
        p->last_value_null_pointer_constant = null_pointer_constant;
        return expected;
      }
      if (tok_same(tok, (ny_ctok_t){.start = "va_arg",
                                    .len = sizeof("va_arg") - 1}) ||
          tok_same(tok, (ny_ctok_t){.start = "__builtin_va_arg",
                                    .len = sizeof("__builtin_va_arg") - 1})) {
        c_value_t list = parse_expr(p, 1);
        c_lvalue_t list_lvalue = {0};
        if (p->abi == NYC_ABI_I386 && list.value >= 0) {
          const c_lvalue_t *lvalue = find_lvalue(p, list.value);
          if (!lvalue) {
            fail(p, "va_arg requires a writable va_list");
            return (c_value_t){-1, C_BAD, NULL};
          }
          list_lvalue = *lvalue;
        }
        if (list.value < 0 ||
            (!c_type_is_pointer(list.type) && list.type != C_VA_LIST) ||
            !expect(p, ","))
          return (c_value_t){-1, C_BAD, NULL};
        c_type_t type;
        if (!parse_type(p, &type, false) || !expect(p, ")"))
          return (c_value_t){-1, C_BAD, NULL};
        c_agg_type_t *type_agg = p->last_type_agg;
        if (p->abi == NYC_ABI_I386) {
          size_t size = type == C_AGG ? type_agg ? type_agg->size : 0
                                      : c_type_size(p->abi, type);
          if (!size || size > INT_MAX - 3 || type == C_F32 ||
              (type != C_AGG && !c_type_is_integer(type) &&
               !c_type_is_pointer(type) && type != C_F64)) {
            fail(p, "va_arg requires a complete promoted argument type");
            return (c_value_t){-1, C_BAD, NULL};
          }
          int value = list.value;
          if (type != C_AGG)
            value = emit_value(
                p, (nyr_inst_t){.op = NYIR_LOAD_I64,
                                .dst = -1,
                                .a = list.value,
                                .flags = size == 4 ? NYIR_INST_F_MEM_I32 : 0,
                                .scalar_class = type == C_F64
                                                    ? NYIR_SCALAR_F64
                                                    : NYIR_SCALAR_I64_RAW});
          int next = lower_add_const(p, list.value, (size + 3u) & ~(size_t)3u);
          if (value < 0 || next < 0 ||
              !c_store_i386_va_list(p, &list_lvalue, next))
            return (c_value_t){-1, C_BAD, NULL};
          if (c_type_is_integer(type))
            value = normalize_integer(p, type, value);
          return (c_value_t){value, type, type_agg};
        }
        if (type == C_AGG) {
          int address = lower_sysv_va_aggregate(p, list.value, type_agg);
          c_value_t result;
          if (address < 0 ||
              !lower_member_rvalue(p, (c_value_t){address, C_AGG, type_agg},
                                   &result))
            return (c_value_t){-1, C_BAD, NULL};
          return result;
        }
        if (!c_type_is_integer(type) && !c_type_is_float(type) &&
            !c_type_is_pointer(type) && type != C_FUNC_PTR) {
          fail(p, "va_arg supports promoted scalar and pointer types");
          return (c_value_t){-1, C_BAD, NULL};
        }
        bool fp = type == C_F64;
        if (type == C_F32) {
          fail(p, "va_arg float must be read as its promoted double type");
          return (c_value_t){-1, C_BAD, NULL};
        }
        int value = emit_value(
            p, (nyr_inst_t){
                   .op = NYIR_VA_ARG,
                   .dst = -1,
                   .a = list.value,
                   .imm = fp ? 1 : 0,
                   .scalar_class = fp ? NYIR_SCALAR_F64 : NYIR_SCALAR_I64_RAW,
                   .effects = NYIR_EFFECT_READ_MEMORY |
                              NYIR_EFFECT_WRITE_MEMORY | NYIR_EFFECT_MAY_TRAP});
        if (value < 0)
          return (c_value_t){-1, C_BAD, NULL};
        if (type == C_I32 || type == C_U32 || type == C_I16 || type == C_U16 ||
            type == C_U8)
          value = normalize_integer(p, type, value);
        return (c_value_t){value, type, type == C_AGG_PTR ? type_agg : NULL};
      }
      c_value_t call_result = parse_call(p, tok);
      if (call_result.value >= 0 &&
          (tok_is(p, p->pos, ".") || tok_is(p, p->pos, "->")) &&
          call_result.type != C_AGG && call_result.type != C_AGG_PTR) {
        char message[96];
        snprintf(message, sizeof(message),
                 "member access on C call result requires aggregate pointer "
                 "(type %d)",
                 call_result.type);
        fail(p, message);
        return (c_value_t){-1, C_BAD, NULL};
      }
      if (call_result.value >= 0 &&
          (call_result.type == C_AGG || call_result.type == C_AGG_PTR)) {
        c_value_t member;
        if (!lower_member_rvalue(p, call_result, &member))
          return (c_value_t){-1, C_BAD, NULL};
        call_result = member;
      }
      return call_result;
    }
    if (tok_is(p, p->pos, "__real__") || tok_is(p, p->pos, "__imag__")) {
      bool imaginary = tok_is(p, p->pos, "__imag__");
      ++p->pos;
      ny_ctok_t object = *current(p);
      c_local_t *object_local = find_local(p, object);
      c_global_t *object_global = object_local ? NULL : find_global(p, object);
      if ((!object_local && !object_global) ||
          !c_type_is_complex(object_local ? object_local->type
                                          : object_global->type)) {
        fail(p, "__real__/__imag__ requires a complex object");
        return (c_value_t){-1, C_BAD, NULL};
      }
      c_type_t object_type =
          object_local ? object_local->type : object_global->type;
      if (object_type == C_C_LDOUBLE) {
        fail(p, "__real__/__imag__ of long double _Complex is not "
                "supported yet");
        return (c_value_t){-1, C_BAD, NULL};
      }
      ++p->pos;
      size_t half = object_type == C_C64 ? 4 : 8;
      int base_address;
      if (object_local) {
        if (c_type_is_complex(object_local->type) && object_local->array_rank) {
          fail(p, "__real__/__imag__ of a complex array is not supported yet");
          return (c_value_t){-1, C_BAD, NULL};
        }
        base_address =
            emit_value(p, (nyr_inst_t){.op = NYIR_ADDR_LOCAL,
                                       .dst = -1,
                                       .imm = (int64_t)object_local->slot,
                                       .scalar_class = NYIR_SCALAR_I64_RAW});
      } else {
        static const char data_prefix[] = "__nyc_data_";
        static const char extern_prefix[] = "__nyc_extern_";
        const char *prefix =
            object_global->is_external || object_global->is_weak ? extern_prefix
                                                                 : data_prefix;
        size_t prefix_len = strlen(prefix);
        size_t symbol_len = prefix_len + object_global->name.len;
        char *symbol = malloc(symbol_len + 1);
        if (!symbol) {
          fail(p, "out of memory lowering __real__/__imag__");
          return (c_value_t){-1, C_BAD, NULL};
        }
        memcpy(symbol, prefix, prefix_len);
        memcpy(symbol + prefix_len, object_global->name.start,
               object_global->name.len);
        symbol[symbol_len] = '\0';
        base_address =
            emit_value(p, (nyr_inst_t){.op = NYIR_ADDR_SYMBOL,
                                       .dst = -1,
                                       .symbol = symbol,
                                       .scalar_class = NYIR_SCALAR_I64_RAW});
        free(symbol);
      }
      int address =
          !imaginary ? base_address
                     : (base_address < 0
                            ? -1
                            : lower_add_const(p, base_address, (int64_t)half));
      if (address < 0)
        return (c_value_t){-1, C_BAD, NULL};
      int loaded = object_type == C_C64
                       ? lower_load_f32_field(p, address, false)
                       : lower_load_field(p, address, true, false);
      c_type_t half_type = object_type == C_C64 ? C_F32 : C_F64;
      if (loaded < 0 || !record_lvalue(p, loaded, address, half_type, false))
        return (c_value_t){-1, C_BAD, NULL};
      return (c_value_t){loaded, half_type, NULL};
    }
    c_local_t *local = find_local(p, tok);
    if (local)
      p->last_type_long_long = local->is_long_long;
    if (!local) {
      c_global_t *global = find_global(p, tok);
      if (global && !global->function_symbol) {
        p->last_type_long_long = global->is_long_long;
        if (c_type_is_complex(global->type)) {
          fail(p, "complex values are not supported yet");
          return (c_value_t){-1, C_BAD, NULL};
        }
        if (global->has_asm_register) {
          if (!global_register_is_x86_stack_pointer(p->abi, global) ||
              (!c_type_is_integer(global->type) &&
               !c_type_is_pointer(global->type))) {
            global_register_use_fails(p, global);
            return (c_value_t){-1, C_BAD, NULL};
          }
          int value =
              emit_value(p, (nyr_inst_t){.op = NYIR_READ_X86_RSP,
                                         .dst = -1,
                                         .scalar_class = NYIR_SCALAR_I64_RAW});
          if (value < 0)
            return (c_value_t){-1, C_BAD, NULL};
          if (c_type_is_integer(global->type))
            value = normalize_integer(p, global->type, value);
          return value < 0 ? (c_value_t){-1, C_BAD, NULL}
                           : (c_value_t){value, global->type, NULL};
        }
        if (global_register_use_fails(p, global))
          return (c_value_t){-1, C_BAD, NULL};
        static const char data_symbol_prefix[] = "__nyc_data_";
        static const char extern_symbol_prefix[] = "__nyc_extern_";
        const char *symbol_prefix = global->is_external || global->is_weak
                                        ? extern_symbol_prefix
                                        : data_symbol_prefix;
        size_t prefix_len = strlen(symbol_prefix);
        size_t symbol_len = prefix_len + global->name.len;
        char *symbol = malloc(symbol_len + 1);
        if (!symbol) {
          fail(p, "out of memory lowering a file-scope data reference");
          return (c_value_t){-1, C_BAD, NULL};
        }
        memcpy(symbol, symbol_prefix, prefix_len);
        memcpy(symbol + prefix_len, global->name.start, global->name.len);
        symbol[symbol_len] = '\0';
        int address = emit_value(
            p,
            (nyr_inst_t){.op = NYIR_ADDR_SYMBOL, .dst = -1, .symbol = symbol});
        free(symbol);
        if (address < 0)
          return (c_value_t){-1, C_BAD, NULL};
        p->last_value_const = global->is_const;
        if (global->type == C_AGG_PTR && tok_is(p, p->pos, "->")) {
          int pointer =
              emit_value(p, (nyr_inst_t){.op = NYIR_LOAD_I64,
                                         .dst = -1,
                                         .a = address,
                                         .effects = NYIR_EFFECT_READ_MEMORY |
                                                    NYIR_EFFECT_MAY_TRAP,
                                         .scalar_class = NYIR_SCALAR_I64_RAW});
          if (pointer < 0)
            return (c_value_t){-1, C_BAD, NULL};
          c_value_t member;
          if (!lower_member_rvalue(
                  p, (c_value_t){pointer, C_AGG_PTR, global->agg}, &member))
            return (c_value_t){-1, C_BAD, NULL};
          return member;
        }
        if (global->type == C_AGG_PTR && !global->is_array && accept(p, "[")) {
          int pointer =
              emit_value(p, (nyr_inst_t){.op = NYIR_LOAD_I64,
                                         .dst = -1,
                                         .a = address,
                                         .effects = NYIR_EFFECT_READ_MEMORY |
                                                    NYIR_EFFECT_MAY_TRAP,
                                         .scalar_class = NYIR_SCALAR_I64_RAW});
          c_value_t index = parse_expr(p, 1);
          if (pointer < 0 || index.value < 0 ||
              !c_type_is_integer(index.type) || !expect(p, "]")) {
            fail(p, "aggregate pointer index must be an integer");
            return (c_value_t){-1, C_BAD, NULL};
          }
          int stride =
              emit_value(p, (nyr_inst_t){.op = NYIR_CONST_I64,
                                         .dst = -1,
                                         .imm = (int64_t)global->agg->size});
          int offset = stride < 0
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
            return (c_value_t){-1, C_BAD, NULL};
          return (c_value_t){element, C_AGG, global->agg};
        }
        if (global->is_array && global->type == C_AGG) {
          if (!accept(p, "["))
            return (c_value_t){address, C_AGG_PTR, global->agg};
          c_value_t index = parse_expr(p, 1);
          if (index.value < 0 || !c_type_is_integer(index.type) ||
              !expect(p, "]")) {
            fail(p, "array index must be an integer");
            return (c_value_t){-1, C_BAD, NULL};
          }
          if (global->array_rank > 1) {
            size_t stride =
                global->array_stride ? global->array_stride : global->agg->size;
            int scale = emit_value(p, (nyr_inst_t){.op = NYIR_CONST_I64,
                                                   .dst = -1,
                                                   .imm = (int64_t)stride});
            int offset = scale < 0
                             ? -1
                             : emit_value(p, (nyr_inst_t){.op = NYIR_MUL_I64,
                                                          .dst = -1,
                                                          .a = index.value,
                                                          .b = scale});
            int row =
                offset < 0
                    ? -1
                    : emit_value(
                          p, (nyr_inst_t){.op = NYIR_ADD_I64,
                                          .dst = -1,
                                          .a = address,
                                          .b = offset,
                                          .flags = c_pointer_offset_flags(p)});
            return row < 0 ? (c_value_t){-1, C_BAD, NULL}
                           : (c_value_t){row, C_AGG_PTR, global->agg};
          }
          int scale =
              emit_value(p, (nyr_inst_t){.op = NYIR_CONST_I64,
                                         .dst = -1,
                                         .imm = (int64_t)global->agg->size});
          if (scale < 0)
            return (c_value_t){-1, C_BAD, NULL};
          int offset =
              emit_value(p, (nyr_inst_t){.op = NYIR_MUL_I64,
                                         .dst = -1,
                                         .a = index.value,
                                         .b = scale,
                                         .flags = c_pointer_offset_flags(p)});
          if (offset < 0)
            return (c_value_t){-1, C_BAD, NULL};
          int element = emit_value(
              p, (nyr_inst_t){
                     .op = NYIR_ADD_I64, .dst = -1, .a = address, .b = offset});
          if (element < 0)
            return (c_value_t){-1, C_BAD, NULL};
          c_value_t member;
          if (!lower_member_rvalue(p, (c_value_t){element, C_AGG, global->agg},
                                   &member))
            return (c_value_t){-1, C_BAD, NULL};
          return member;
        }
        if (global->is_array && accept(p, "[")) {
          c_value_t index = parse_expr(p, 1);
          if (index.value < 0 || !c_type_is_integer(index.type) ||
              !expect(p, "]")) {
            fail(p, "array index must be a signed integer");
            return (c_value_t){-1, C_BAD, NULL};
          }
          c_type_t array_type =
              global->is_pointer_array ? C_OPAQUE_PTR
              : global->type == C_U8 || global->type == C_CHAR ||
                      global->type == C_I16 || global->type == C_U16 ||
                      global->type == C_I32 || global->type == C_U32
                  ? c_array_pointer_type(global->type)
                  : global->type;
          size_t base_element_size = c_type_size(p->abi, global->type);
          if (global->is_pointer_array && global->array_rank > 1) {
            int stride = emit_value(
                p, (nyr_inst_t){.op = NYIR_CONST_I64,
                                .dst = -1,
                                .imm = (int64_t)global->array_stride});
            int offset = stride < 0
                             ? -1
                             : emit_value(p, (nyr_inst_t){.op = NYIR_MUL_I64,
                                                          .dst = -1,
                                                          .a = index.value,
                                                          .b = stride});
            int row =
                offset < 0
                    ? -1
                    : emit_value(
                          p, (nyr_inst_t){.op = NYIR_ADD_I64,
                                          .dst = -1,
                                          .a = address,
                                          .b = offset,
                                          .flags = c_pointer_offset_flags(p)});
            return row < 0
                       ? (c_value_t){-1, C_BAD, NULL}
                       : (c_value_t){row,
                                     !global->is_pointer_array &&
                                             global->type == C_AGG_PTR
                                         ? C_AGG_PTR
                                         : c_array_pointer_type(global->type),
                                     global->agg};
          }
          /*
           * A one-dimensional array of structs selects an element in place;
           * returning a row pointer would lose the aggregate lvalue that
           * parenthesized assignments and `swap()` need.
           */
          if (!global->is_pointer_array && global->type == C_AGG &&
              global->agg && global->array_rank <= 1 &&
              global->array_shape.rank <= 1) {
            int element_address = lower_pointer_offset_sized(
                p, (int64_t)global->agg->size, address, index.value, false);
            if (element_address < 0 ||
                !record_lvalue(p, element_address, element_address, C_AGG,
                               global->is_volatile))
              return (c_value_t){-1, C_BAD, NULL};
            p->lvalues[p->lvalue_count - 1].agg = global->agg;
            return (c_value_t){element_address, C_AGG, global->agg};
          }
          if (!global->is_pointer_array &&
              (global->array_rank > 1 || global->array_shape.rank > 1 ||
               global->array_stride > base_element_size)) {
            int stride = emit_value(
                p, (nyr_inst_t){.op = NYIR_CONST_I64,
                                .dst = -1,
                                .imm = (int64_t)global->array_stride});
            int offset = stride < 0
                             ? -1
                             : emit_value(p, (nyr_inst_t){.op = NYIR_MUL_I64,
                                                          .dst = -1,
                                                          .a = index.value,
                                                          .b = stride});
            int row = offset < 0
                          ? -1
                          : emit_value(p, (nyr_inst_t){.op = NYIR_ADD_I64,
                                                       .dst = -1,
                                                       .a = address,
                                                       .b = offset});
            if (row < 0)
              return (c_value_t){-1, C_BAD, NULL};
            c_array_shape_t shape = global->array_shape;
            if (shape.rank > 1) {
              for (unsigned dimension = 1; dimension < shape.rank; ++dimension)
                shape.extent[dimension - 1] = shape.extent[dimension];
              --shape.rank;
              if (!remember_array_shape(p, row, shape))
                return (c_value_t){-1, C_BAD, NULL};
            }
            return (c_value_t){
                row, c_array_pointer_type(global->type),
                global->is_volatile
                    ? c_pointee_marker(global->is_const, true, 0)
                    : NULL};
          }
          int element_address =
              lower_pointer_index_address(p, array_type, address, index.value);
          if (element_address < 0)
            return (c_value_t){-1, C_BAD, NULL};
          if (global->is_pointer_array) {
            int element = emit_value(
                p, (nyr_inst_t){.op = NYIR_LOAD_I64,
                                .dst = -1,
                                .a = element_address,
                                .effects = NYIR_EFFECT_READ_MEMORY |
                                           NYIR_EFFECT_MAY_TRAP,
                                .scalar_class = NYIR_SCALAR_I64_RAW});
            c_type_t element_type = global->type == C_AGG_PTR ? C_AGG_PTR
                                    : global->type == C_FUNC_PTR ||
                                            global->is_function_pointer_array
                                        ? C_FUNC_PTR
                                        : C_CHAR_PTR;
            if (element < 0 || !record_lvalue(p, element, element_address,
                                              element_type, false))
              return (c_value_t){-1, C_BAD, NULL};
            if (global->type == C_AGG_PTR)
              p->lvalues[p->lvalue_count - 1].agg = global->agg;
            return (c_value_t){element, element_type,
                               global->type == C_AGG_PTR ? global->agg : NULL};
          }
          if (array_type == C_U16_PTR || array_type == C_I16_PTR) {
            if (global->is_volatile) {
              c_type_t type = array_type == C_I16_PTR ? C_I16 : C_U16;
              int loaded = lower_load_u16_field(p, element_address, type, true);
              if (loaded < 0 ||
                  !record_lvalue(p, loaded, element_address, type, true))
                return (c_value_t){-1, C_BAD, NULL};
              return (c_value_t){loaded, type, NULL};
            }
            int one = emit_value(
                p, (nyr_inst_t){.op = NYIR_CONST_I64, .dst = -1, .imm = 1});
            int high_address =
                one < 0 ? -1
                        : emit_value(p, (nyr_inst_t){.op = NYIR_ADD_I64,
                                                     .dst = -1,
                                                     .a = element_address,
                                                     .b = one});
            int low = emit_value(
                p, (nyr_inst_t){.op = NYIR_LOAD_I64,
                                .dst = -1,
                                .a = element_address,
                                .effects = NYIR_EFFECT_READ_MEMORY |
                                           NYIR_EFFECT_MAY_TRAP,
                                .flags = NYIR_INST_F_MEM_BYTE,
                                .scalar_class = NYIR_SCALAR_I64_RAW});
            int high =
                high_address < 0
                    ? -1
                    : emit_value(
                          p, (nyr_inst_t){.op = NYIR_LOAD_I64,
                                          .dst = -1,
                                          .a = high_address,
                                          .effects = NYIR_EFFECT_READ_MEMORY |
                                                     NYIR_EFFECT_MAY_TRAP,
                                          .flags = NYIR_INST_F_MEM_BYTE,
                                          .scalar_class = NYIR_SCALAR_I64_RAW});
            int shift = emit_value(
                p, (nyr_inst_t){.op = NYIR_CONST_I64, .dst = -1, .imm = 8});
            int shifted = high < 0 || shift < 0
                              ? -1
                              : emit_value(p, (nyr_inst_t){.op = NYIR_SHL_I64,
                                                           .dst = -1,
                                                           .a = high,
                                                           .b = shift});
            int combined = low < 0 || shifted < 0
                               ? -1
                               : emit_value(p, (nyr_inst_t){.op = NYIR_OR_I64,
                                                            .dst = -1,
                                                            .a = low,
                                                            .b = shifted});
            if (combined < 0)
              return (c_value_t){-1, C_BAD, NULL};
            if (array_type == C_I16_PTR) {
              int sign_shift = emit_value(
                  p, (nyr_inst_t){.op = NYIR_CONST_I64, .dst = -1, .imm = 48});
              int left = sign_shift < 0
                             ? -1
                             : emit_value(p, (nyr_inst_t){.op = NYIR_SHL_I64,
                                                          .dst = -1,
                                                          .a = combined,
                                                          .b = sign_shift});
              int right = left < 0 || sign_shift < 0
                              ? -1
                              : emit_value(p, (nyr_inst_t){.op = NYIR_SAR_I64,
                                                           .dst = -1,
                                                           .a = left,
                                                           .b = sign_shift});
              if (right < 0)
                return (c_value_t){-1, C_BAD, NULL};
              return (c_value_t){right, C_I16, NULL};
            }
            return (c_value_t){combined, C_U16, NULL};
          }
          if (array_type == C_I32_PTR || array_type == C_U32_PTR) {
            int loaded = emit_value(
                p, (nyr_inst_t){.op = NYIR_LOAD_I64,
                                .dst = -1,
                                .a = element_address,
                                .effects = NYIR_EFFECT_READ_MEMORY |
                                           NYIR_EFFECT_MAY_TRAP,
                                .flags = NYIR_INST_F_MEM_I32,
                                .scalar_class = NYIR_SCALAR_I64_RAW});
            if (loaded < 0)
              return (c_value_t){-1, C_BAD, NULL};
            int value = array_type == C_I32_PTR ? normalize_i32(p, loaded)
                                                : normalize_u32(p, loaded);
            c_type_t element_type = array_type == C_I32_PTR ? C_I32 : C_U32;
            if (value < 0 ||
                !record_lvalue(p, value, element_address, element_type,
                               global->is_volatile))
              return (c_value_t){-1, C_BAD, NULL};
            return (c_value_t){value, element_type, NULL};
          }
          if (global->type == C_I64 || global->type == C_U64) {
            int value = emit_value(
                p, (nyr_inst_t){.op = NYIR_LOAD_I64,
                                .dst = -1,
                                .a = element_address,
                                .effects = NYIR_EFFECT_READ_MEMORY |
                                           NYIR_EFFECT_MAY_TRAP,
                                .scalar_class = NYIR_SCALAR_I64_RAW});
            if (value < 0 || !record_lvalue(p, value, element_address,
                                            global->type, global->is_volatile))
              return (c_value_t){-1, C_BAD, NULL};
            return (c_value_t){value, global->type, global->agg};
          }
          int loaded =
              emit_value(p, (nyr_inst_t){.op = NYIR_LOAD_I64,
                                         .dst = -1,
                                         .a = element_address,
                                         .effects = NYIR_EFFECT_READ_MEMORY |
                                                    NYIR_EFFECT_MAY_TRAP,
                                         .flags = NYIR_INST_F_MEM_BYTE,
                                         .scalar_class = NYIR_SCALAR_I64_RAW});
          if (loaded < 0)
            return (c_value_t){-1, C_BAD, NULL};
          c_type_t element_type = global->is_signed_array ? C_CHAR : C_U8;
          if (global->is_signed_array) {
            int shift = emit_value(
                p, (nyr_inst_t){.op = NYIR_CONST_I64, .dst = -1, .imm = 56});
            int left = shift < 0
                           ? -1
                           : emit_value(p, (nyr_inst_t){.op = NYIR_SHL_I64,
                                                        .dst = -1,
                                                        .a = loaded,
                                                        .b = shift});
            int right = left < 0 || shift < 0
                            ? -1
                            : emit_value(p, (nyr_inst_t){.op = NYIR_SAR_I64,
                                                         .dst = -1,
                                                         .a = left,
                                                         .b = shift});
            if (right < 0)
              return (c_value_t){-1, C_BAD, NULL};
            loaded = right;
          }
          if (!record_lvalue(p, loaded, element_address, element_type,
                             global->is_volatile))
            return (c_value_t){-1, C_BAD, NULL};
          return (c_value_t){loaded, element_type, NULL};
        }
        if (global->is_array)
          return (c_value_t){
              address,
              global->is_pointer_array
                  ? global->type == C_AGG_PTR ? C_AGG_PTR_PTR : C_OPAQUE_PTR
              : global->type == C_CHAR_PTR ? C_CHAR_PTR
              : global->type == C_AGG ? C_AGG_ARRAY_PTR
                                      : c_add_pointer_levels(global->type, 1),
              global->type == C_AGG_PTR ? global->agg
              : !global->is_pointer_array && global->is_volatile &&
                        c_type_is_integer(global->type)
                  ? c_pointee_marker(global->is_const, true, 0)
                  : NULL};
        if (global->type == C_AGG) {
          c_value_t member;
          if (!lower_member_rvalue(p, (c_value_t){address, C_AGG, global->agg},
                                   &member))
            return (c_value_t){-1, C_BAD, NULL};
          return member;
        }
        uint8_t memory_flags =
            global->type == C_I32 || global->type == C_U32
                ? NYIR_INST_F_MEM_I32
                : (global->type == C_U8 || global->type == C_CHAR
                       ? NYIR_INST_F_MEM_BYTE
                       : 0);
        int value =
            global->object_seg
                ? lower_load_segmented_global(p, global, address)
                : emit_value(p, (nyr_inst_t){
                                    .op = NYIR_LOAD_I64,
                                    .dst = -1,
                                    .a = address,
                                    .effects = NYIR_EFFECT_READ_MEMORY |
                                               NYIR_EFFECT_MAY_TRAP,
                                    .flags = memory_flags,
                                    .scalar_class = global->type == C_F64
                                                        ? NYIR_SCALAR_F64
                                                        : NYIR_SCALAR_I64_RAW});
        if (c_type_is_integer(global->type) && value >= 0)
          value = normalize_integer(p, global->type, value);
        if (value < 0)
          return (c_value_t){-1, C_BAD, NULL};
        c_type_t value_type =
            global->pointer_depth > 1 && global->pointer_base_type == C_AGG
                ? c_add_pointer_levels(global->pointer_base_type,
                                       global->pointer_depth)
                : global->type;
        if ((value_type == C_OPAQUE_PTR_PTR ||
             value_type == C_OPAQUE_PTR_PTR_PTR ||
             value_type == C_CHAR_PTR_PTR || value_type == C_I8_PTR_PTR ||
             value_type == C_FUNC_PTR_PTR || value_type == C_AGG_PTR_PTR) &&
            tok_is(p, p->pos, "["))
          return lower_pointer_index_rvalue(
              p, (c_value_t){value, value_type,
                             global->agg
                                 ? global->agg
                                 : c_pointee_marker(false, global->is_volatile,
                                                    global->pointee_seg)});
        if (c_type_is_arithmetic_pointer(value_type) && accept(p, "[")) {
          c_value_t index = parse_expr(p, 1);
          if (index.value < 0 || !c_type_is_integer(index.type) ||
              !expect(p, "]")) {
            fail(p, "array index must be a signed integer");
            return (c_value_t){-1, C_BAD, NULL};
          }
          int element_address =
              lower_pointer_index_address(p, value_type, value, index.value);
          if (element_address < 0)
            return (c_value_t){-1, C_BAD, NULL};
          if (value_type == C_AGG_ARRAY_PTR)
            return (c_value_t){element_address, C_AGG_PTR, global->agg};
          c_type_t element_type = value_type == C_CHAR_PTR  ? C_U8
                                  : value_type == C_I8_PTR  ? C_CHAR
                                  : value_type == C_I32_PTR ? C_I32
                                  : value_type == C_U32_PTR ? C_U32
                                  : value_type == C_F64_PTR ? C_F64
                                                            : C_I64;
          unsigned flags = (value_type == C_CHAR_PTR || value_type == C_I8_PTR
                                ? NYIR_INST_F_MEM_BYTE
                            : value_type == C_I32_PTR || value_type == C_U32_PTR
                                ? NYIR_INST_F_MEM_I32
                                : 0) |
                           c_seg_flags(global->pointee_seg);
          int element = emit_value(
              p, (nyr_inst_t){.op = NYIR_LOAD_I64,
                              .dst = -1,
                              .a = element_address,
                              .effects = NYIR_EFFECT_READ_MEMORY |
                                         NYIR_EFFECT_MAY_TRAP,
                              .flags = flags,
                              .scalar_class = element_type == C_F64
                                                  ? NYIR_SCALAR_F64
                                                  : NYIR_SCALAR_I64_RAW});
          if (element >= 0 && c_type_is_integer(element_type))
            element = normalize_integer(p, element_type, element);
          if (element < 0 || !record_lvalue(p, element, element_address,
                                            element_type, global->is_volatile))
            return (c_value_t){-1, C_BAD, NULL};
          return (c_value_t){element, element_type, NULL};
        }
        p->last_value_const = global->is_const;
        if (global->type == C_FUNC_PTR) {
          p->last_type_function_result_type = global->function_result_type;
          p->last_type_function_result_agg = global->agg;
          p->last_type_function_type = true;
        }
        if (!record_lvalue(p, value, address, value_type, global->is_volatile))
          return (c_value_t){-1, C_BAD, NULL};
        p->lvalues[p->lvalue_count - 1].segment_flags =
            c_seg_flags(global->object_seg);
        return (c_value_t){value, value_type,
                           global->agg
                               ? global->agg
                               : c_pointee_marker(false, global->is_volatile,
                                                  global->pointee_seg)};
      }
      c_function_t *function = find_function(p, tok);
      if (function) {
        if (function->has_asm_alias && !function->asm_alias.len) {
          fail(p, "function asm label must be a string literal");
          return (c_value_t){-1, C_BAD, NULL};
        }
        static const char extern_prefix[] = "__nyc_extern_";
        ny_ctok_t link_name =
            function->asm_alias.len ? function->asm_alias : function->name;
        size_t prefix_len = function->asm_alias.len || function->defined
                                ? 0
                                : sizeof(extern_prefix) - 1;
        if (link_name.len > SIZE_MAX - prefix_len - 1) {
          fail(p, "function reference symbol is too long");
          return (c_value_t){-1, C_BAD, NULL};
        }
        char *symbol = malloc(prefix_len + link_name.len + 1);
        if (!symbol) {
          fail(p, "out of memory lowering a function reference");
          return (c_value_t){-1, C_BAD, NULL};
        }
        if (prefix_len)
          memcpy(symbol, extern_prefix, prefix_len);
        memcpy(symbol + prefix_len, link_name.start, link_name.len);
        symbol[prefix_len + link_name.len] = '\0';
        int address = emit_value(
            p,
            (nyr_inst_t){.op = NYIR_ADDR_SYMBOL, .dst = -1, .symbol = symbol});
        free(symbol);
        return address < 0 ? (c_value_t){-1, C_BAD, NULL}
                           : (c_value_t){address, C_FUNC_PTR, NULL};
      }
      char message[128];
      snprintf(message, sizeof(message),
               "reference `%.*s` must name a function parameter or declared "
               "local",
               (int)(tok.len > 40 ? 40 : tok.len), tok.start);
      fail(p, message);
      return (c_value_t){-1, C_BAD, NULL};
    }
    size_t local_read_start = p->out->len;
    int value = lower_local_load(p, local);
    p->last_value_const = local->is_const;
    p->last_value_pointee_const = local->pointee_const;
    if (value < 0)
      return (c_value_t){-1, C_BAD, NULL};
    if (!remember_array_shape(p, value, local->pointee_array))
      return (c_value_t){-1, C_BAD, NULL};
    if (!local->array_rank && local->type != C_VA_LIST &&
        !record_local_lvalue(p, value, local, local_read_start))
      return (c_value_t){-1, C_BAD, NULL};
    if (local->array_rank) {
      int offset = -1;
      unsigned indexed = 0;
      while (indexed < local->array_rank && accept(p, "[")) {
        c_value_t index = parse_expr(p, 1);
        if (index.value < 0 || !c_type_is_integer(index.type) ||
            !expect(p, "]")) {
          fail(p, "static array index must be an integer");
          return (c_value_t){-1, C_BAD, NULL};
        }
        size_t stride = c_type_size(p->abi, local->type);
        for (unsigned dim = indexed + 1; dim < local->array_rank; ++dim)
          stride *= local->array_extent[dim];
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
          return (c_value_t){-1, C_BAD, NULL};
        ++indexed;
      }
      if (!indexed)
        return (c_value_t){value, c_array_pointer_type(local->type), NULL};
      int address = offset < 0 ? value
                               : emit_value(p, (nyr_inst_t){.op = NYIR_ADD_I64,
                                                            .dst = -1,
                                                            .a = value,
                                                            .b = offset});
      if (address < 0)
        return (c_value_t){-1, C_BAD, NULL};
      p->last_value_const = local->is_const && !c_type_is_pointer(local->type);
      if (indexed < local->array_rank)
        return (c_value_t){address, c_array_pointer_type(local->type), NULL};
      int loaded = emit_value(
          p, (nyr_inst_t){
                 .op = NYIR_LOAD_I64,
                 .dst = -1,
                 .a = address,
                 .effects = NYIR_EFFECT_READ_MEMORY | NYIR_EFFECT_MAY_TRAP |
                            (local->is_volatile ? NYIR_EFFECT_VOLATILE : 0),
                 .flags = c_local_memory_flags(p, local->type),
                 .scalar_class = c_type_is_float(local->type)
                                     ? c_float_scalar_class(local->type)
                                     : NYIR_SCALAR_I64_RAW});
      if (loaded >= 0 && c_type_is_integer(local->type))
        loaded = normalize_integer(p, local->type, loaded);
      if (loaded < 0 ||
          !record_lvalue(p, loaded, address, local->type, local->is_volatile))
        return (c_value_t){-1, C_BAD, NULL};
      /*
       * An element of a function-pointer array is still a function pointer;
       * carry its result signature so a following call does not fall back to
       * an implicit int result.
       */
      if (local->type == C_FUNC_PTR) {
        p->indirect_bool_params = 0; /* set below only for sources that know it */
        p->indirect_result_type = local->function_result_type;
        p->indirect_result_agg = local->agg;
        p->has_indirect_result_signature = local->function_result_type != C_BAD;
        p->indirect_bool_params = local->function_bool_params;
        p->last_type_function_result_type = local->function_result_type;
        p->last_type_function_result_agg = local->agg;
        p->last_type_function_type = true;
      }
      if (local->type == C_AGG_PTR && local->agg &&
          (tok_is(p, p->pos, "->") || tok_is(p, p->pos, "."))) {
        c_value_t member;
        if (!lower_member_rvalue(p, (c_value_t){loaded, C_AGG_PTR, local->agg},
                                 &member))
          return (c_value_t){-1, C_BAD, NULL};
        return member;
      }
      if (local->type == C_CHAR_PTR && accept(p, "[")) {
        c_value_t char_index = parse_expr(p, 1);
        if (char_index.value < 0 || !c_type_is_integer(char_index.type) ||
            !expect(p, "]")) {
          fail(p, "string index must be an integer");
          return (c_value_t){-1, C_BAD, NULL};
        }
        int char_address = lower_pointer_index_address(p, C_CHAR_PTR, loaded,
                                                       char_index.value);
        int character =
            char_address < 0
                ? -1
                : emit_value(p,
                             (nyr_inst_t){.op = NYIR_LOAD_I64,
                                          .dst = -1,
                                          .a = char_address,
                                          .effects = NYIR_EFFECT_READ_MEMORY |
                                                     NYIR_EFFECT_MAY_TRAP,
                                          .flags = NYIR_INST_F_MEM_BYTE,
                                          .scalar_class = NYIR_SCALAR_I64_RAW});
        if (character < 0 ||
            !record_lvalue(p, character, char_address, C_U8, local->is_const))
          return (c_value_t){-1, C_BAD, NULL};
        p->last_value_const = local->is_const;
        return (c_value_t){character, C_U8, NULL};
      }
      return (c_value_t){loaded, local->type, NULL};
    }
    if (local->type == C_AGG_PTR && local->agg && accept(p, "[")) {
      c_value_t index = parse_expr(p, 1);
      if (index.value < 0 || !c_type_is_integer(index.type) ||
          !expect(p, "]")) {
        fail(p, "aggregate pointer index must be an integer");
        return (c_value_t){-1, C_BAD, NULL};
      }
      int address = lower_pointer_offset_sized(p, (int64_t)local->agg->size,
                                               value, index.value, false);
      if (address < 0)
        return (c_value_t){-1, C_BAD, NULL};
      p->last_value_const = local->is_const && !c_type_is_pointer(local->type);
      /*
       * The element is addressed in place; record it so a following
       * assignment or member store can use it as a modifiable lvalue.
       */
      if (!record_lvalue(p, address, address, C_AGG, local->is_volatile))
        return (c_value_t){-1, C_BAD, NULL};
      p->lvalues[p->lvalue_count - 1].agg = local->agg;
      c_value_t element = {address, C_AGG, local->agg};
      c_value_t member;
      if (!lower_member_rvalue(p, element, &member))
        return (c_value_t){-1, C_BAD, NULL};
      return member;
    }
    if (local->type == C_VA_LIST)
      return (c_value_t){value, C_OPAQUE_PTR, NULL};
    if (local->type == C_AGG && local->array_count > 0) {
      /*
       * Array of structs: index to a pointer to the selected element.
       */
      if (!accept(p, "["))
        return (c_value_t){value, C_AGG_PTR, local->agg};
      c_value_t index = parse_expr(p, 1);
      if (index.value < 0 || !c_type_is_integer(index.type) ||
          !expect(p, "]")) {
        fail(p, "array index must be a signed integer");
        return (c_value_t){-1, C_BAD, NULL};
      }
      int scale = emit_value(p, (nyr_inst_t){.op = NYIR_CONST_I64,
                                             .dst = -1,
                                             .imm = (int64_t)local->agg->size});
      if (scale < 0)
        return (c_value_t){-1, C_BAD, NULL};
      int offset = emit_value(
          p, (nyr_inst_t){
                 .op = NYIR_MUL_I64, .dst = -1, .a = index.value, .b = scale});
      if (offset < 0)
        return (c_value_t){-1, C_BAD, NULL};
      int address = emit_value(
          p,
          (nyr_inst_t){.op = NYIR_ADD_I64, .dst = -1, .a = value, .b = offset});
      if (address < 0)
        return (c_value_t){-1, C_BAD, NULL};
      /*
       * The selected struct element lives in place; record it so a
       * parenthesized assignment such as the `swap()` macro can modify it.
       */
      if (!record_lvalue(p, address, address, C_AGG, local->is_volatile))
        return (c_value_t){-1, C_BAD, NULL};
      p->lvalues[p->lvalue_count - 1].agg = local->agg;
      c_value_t element = {address, C_AGG, local->agg};
      c_value_t member;
      if (!lower_member_rvalue(p, element, &member))
        return (c_value_t){-1, C_BAD, NULL};
      return member;
    }
    if (local->pointee_array.rank && array_shape_for_value(p, value) &&
        tok_is(p, p->pos, "[")) {
      return (c_value_t){value, local->type, local->agg};
    }
    if (accept(p, "[")) {
      if (local->type == C_FUNC_PTR_PTR || local->type == C_OPAQUE_PTR_PTR ||
          local->type == C_U16_PTR_PTR || local->type == C_AGG_PTR_PTR ||
          local->type == C_AGG_PTR_ARRAY_PTR) {
        c_value_t index = parse_expr(p, 1);
        if (index.value < 0 || !c_type_is_integer(index.type) ||
            !expect(p, "]")) {
          fail(p, "pointer-to-pointer index must be an integer");
          return (c_value_t){-1, C_BAD, NULL};
        }
        int address =
            lower_pointer_index_address(p, C_OPAQUE_PTR, value, index.value);
        int loaded =
            address < 0
                ? -1
                : emit_value(p,
                             (nyr_inst_t){.op = NYIR_LOAD_I64,
                                          .dst = -1,
                                          .a = address,
                                          .effects = NYIR_EFFECT_READ_MEMORY |
                                                     NYIR_EFFECT_MAY_TRAP,
                                          .scalar_class = NYIR_SCALAR_I64_RAW});
        if (loaded < 0)
          return (c_value_t){-1, C_BAD, NULL};
        c_value_t element = {
            loaded,
            local->type == C_FUNC_PTR_PTR        ? C_FUNC_PTR
            : local->type == C_U16_PTR_PTR       ? C_U16_PTR
            : local->type == C_AGG_PTR_ARRAY_PTR ? C_AGG_ARRAY_PTR
            : local->type == C_AGG_PTR_PTR
                ? local->pointee_array_size ? C_AGG_ARRAY_PTR : C_AGG_PTR
            : local->type == C_OPAQUE_PTR_PTR &&
                    local->pointer_base_type == C_AGG &&
                    local->pointee_array_size
                ? C_AGG_ARRAY_PTR
            : local->type == C_OPAQUE_PTR_PTR && local->pointer_depth > 1
                ? c_add_pointer_levels(local->pointer_base_type,
                                       local->pointer_depth - 1)
                : C_OPAQUE_PTR,
            local->type == C_AGG_PTR_PTR ||
                    local->type == C_AGG_PTR_ARRAY_PTR ||
                    (local->type == C_OPAQUE_PTR_PTR &&
                     local->pointer_base_type == C_AGG &&
                     local->pointee_array_size)
                ? local->agg
                : NULL};
        if (!record_lvalue(p, loaded, address, element.type,
                           local->pointee_volatile))
          return (c_value_t){-1, C_BAD, NULL};
        if (element.agg)
          p->lvalues[p->lvalue_count - 1].agg = element.agg;
        if ((local->type == C_AGG_PTR_PTR ||
             local->type == C_AGG_PTR_ARRAY_PTR ||
             (local->type == C_OPAQUE_PTR_PTR &&
              local->pointer_base_type == C_AGG &&
              local->pointee_array_size)) &&
            (tok_is(p, p->pos, "->") || tok_is(p, p->pos, "."))) {
          c_value_t member;
          if (!lower_member_rvalue(p, element, &member))
            return (c_value_t){-1, C_BAD, NULL};
          return member;
        }
        return element;
      }
      if (local->type == C_CHAR_PTR_PTR || local->type == C_I8_PTR_PTR) {
        c_value_t index = parse_expr(p, 1);
        if (index.value < 0 || !c_type_is_integer(index.type) ||
            !expect(p, "]")) {
          fail(p, "pointer-array index must be an integer");
          return (c_value_t){-1, C_BAD, NULL};
        }
        int address =
            lower_pointer_index_address(p, C_OPAQUE_PTR, value, index.value);
        int loaded =
            address < 0
                ? -1
                : emit_value(p,
                             (nyr_inst_t){.op = NYIR_LOAD_I64,
                                          .dst = -1,
                                          .a = address,
                                          .effects = NYIR_EFFECT_READ_MEMORY |
                                                     NYIR_EFFECT_MAY_TRAP,
                                          .scalar_class = NYIR_SCALAR_I64_RAW});
        if (loaded < 0)
          return (c_value_t){-1, C_BAD, NULL};
        if (accept(p, "[")) {
          c_value_t char_index = parse_expr(p, 1);
          if (char_index.value < 0 || !c_type_is_integer(char_index.type) ||
              !expect(p, "]")) {
            fail(p, "string index must be an integer");
            return (c_value_t){-1, C_BAD, NULL};
          }
          c_type_t string_pointer_type =
              local->type == C_I8_PTR_PTR ? C_I8_PTR : C_CHAR_PTR;
          int char_address = lower_pointer_index_address(
              p, string_pointer_type, loaded, char_index.value);
          if (char_address < 0)
            return (c_value_t){-1, C_BAD, NULL};
          int character =
              emit_value(p, (nyr_inst_t){.op = NYIR_LOAD_I64,
                                         .dst = -1,
                                         .a = char_address,
                                         .effects = NYIR_EFFECT_READ_MEMORY |
                                                    NYIR_EFFECT_MAY_TRAP,
                                         .flags = NYIR_INST_F_MEM_BYTE,
                                         .scalar_class = NYIR_SCALAR_I64_RAW});
          c_type_t character_type = local->type == C_I8_PTR_PTR ? C_CHAR : C_U8;
          if (character >= 0 && character_type == C_CHAR)
            character = normalize_integer(p, character_type, character);
          return character < 0 ? (c_value_t){-1, C_BAD, NULL}
                               : (c_value_t){character, character_type, NULL};
        }
        return (c_value_t){
            loaded, local->type == C_I8_PTR_PTR ? C_I8_PTR : C_CHAR_PTR, NULL};
      }
      if (local->type == C_OPAQUE_PTR || local->type == C_OPAQUE_PTR_PTR ||
          local->type == C_OPAQUE_PTR_PTR_PTR) {
        c_value_t index = parse_expr(p, 1);
        if (index.value < 0 || !c_type_is_integer(index.type) ||
            !expect(p, "]")) {
          fail(p, "pointer-array index must be an integer");
          return (c_value_t){-1, C_BAD, NULL};
        }
        int address =
            lower_pointer_index_address(p, local->type, value, index.value);
        int loaded =
            address < 0
                ? -1
                : emit_value(p,
                             (nyr_inst_t){.op = NYIR_LOAD_I64,
                                          .dst = -1,
                                          .a = address,
                                          .effects = NYIR_EFFECT_READ_MEMORY |
                                                     NYIR_EFFECT_MAY_TRAP,
                                          .scalar_class = NYIR_SCALAR_I64_RAW});
        c_type_t element_type = c_remove_pointer_levels(local->type, 1);
        if (loaded < 0 || element_type == C_BAD)
          return (c_value_t){-1, C_BAD, NULL};
        if (!record_lvalue(p, loaded, address, element_type,
                           local->pointee_volatile))
          return (c_value_t){-1, C_BAD, NULL};
        return (c_value_t){loaded, element_type, NULL};
      }
      if (local->type == C_AGG_ARRAY_PTR && local->agg) {
        c_value_t index = parse_expr(p, 1);
        if (index.value < 0 || !c_type_is_integer(index.type) ||
            !expect(p, "]")) {
          fail(p, "aggregate-array pointer index must be an integer");
          return (c_value_t){-1, C_BAD, NULL};
        }
        int address =
            lower_pointer_index_address(p, local->type, value, index.value);
        return address < 0 ? (c_value_t){-1, C_BAD, NULL}
                           : (c_value_t){address, C_AGG_ARRAY_PTR, local->agg};
      }
      if (local->type != C_CHAR_PTR && local->type != C_I8_PTR &&
          local->type != C_I32_PTR && local->type != C_U32_PTR &&
          local->type != C_I16_PTR && local->type != C_U16_PTR &&
          local->type != C_I64_PTR && local->type != C_U64_PTR &&
          local->type != C_F32_PTR && local->type != C_F64_PTR) {
        fail(p, "array indexing requires a pointer to char, int, long, or "
                "double");
        return (c_value_t){-1, C_BAD, NULL};
      }
      c_value_t index = parse_expr(p, 1);
      if (index.value < 0 || !c_type_is_integer(index.type) ||
          !expect(p, "]")) {
        fail(p, "array index must be a signed integer");
        return (c_value_t){-1, C_BAD, NULL};
      }
      int address =
          lower_pointer_index_address(p, local->type, value, index.value);
      if (address < 0)
        return (c_value_t){-1, C_BAD, NULL};
      p->last_value_const = local->is_const && !c_type_is_pointer(local->type);
      c_type_t element_type = local->type == C_CHAR_PTR  ? C_U8
                              : local->type == C_I8_PTR  ? C_CHAR
                              : local->type == C_I16_PTR ? C_I16
                              : local->type == C_U16_PTR ? C_U16
                              : local->type == C_I32_PTR ? C_I32
                              : local->type == C_U32_PTR ? C_U32
                              : local->type == C_U64_PTR ? C_U64
                              : local->type == C_F32_PTR ? C_F32
                              : local->type == C_F64_PTR ? C_F64
                                                         : C_I64;
      unsigned memory_flags =
          ((local->type == C_CHAR_PTR || local->type == C_I8_PTR
                ? NYIR_INST_F_MEM_BYTE
                : (local->type == C_I32_PTR || local->type == C_U32_PTR ||
                           local->type == C_F32_PTR
                       ? NYIR_INST_F_MEM_I32
                       : 0)) |
           c_seg_flags(local->pointee_seg));
      int loaded =
          local->type == C_I16_PTR || local->type == C_U16_PTR
              ? lower_load_u16_field_seg(p, address, element_type,
                                         local->pointee_volatile,
                                         c_seg_flags(local->pointee_seg))
              : emit_value(
                    p, (nyr_inst_t){.op = NYIR_LOAD_I64,
                                    .dst = -1,
                                    .a = address,
                                    .effects = NYIR_EFFECT_READ_MEMORY |
                                               NYIR_EFFECT_MAY_TRAP |
                                               (local->pointee_volatile
                                                    ? NYIR_EFFECT_VOLATILE
                                                    : 0),
                                    .flags = memory_flags,
                                    .scalar_class =
                                        c_type_is_float(element_type)
                                            ? c_float_scalar_class(element_type)
                                            : NYIR_SCALAR_I64_RAW});
      if (loaded < 0)
        return (c_value_t){-1, C_BAD, NULL};
      if (c_type_is_integer(element_type))
        loaded = normalize_integer(p, element_type, loaded);
      if (!record_lvalue(p, loaded, address, element_type,
                         local->pointee_volatile))
        return (c_value_t){-1, C_BAD, NULL};
      if (accept(p, "++") || accept(p, "--")) {
        bool decrement = ny_ctok_eq(p->tokens[p->pos - 1], "--");
        if (local->is_const || (!c_type_is_integer(element_type) &&
                                !c_type_is_float(element_type))) {
          fail(p, "postfix increment requires a mutable numeric element");
          return (c_value_t){-1, C_BAD, NULL};
        }
        int one = emit_value(
            p, c_type_is_float(element_type)
                   ? (nyr_inst_t){.op = element_type == C_F32 ? NYIR_CONST_F32
                                                              : NYIR_CONST_F64,
                                  .dst = -1,
                                  .imm = element_type == C_F32
                                             ? (int64_t)0x3f800000
                                             : INT64_C(0x3ff0000000000000),
                                  .scalar_class =
                                      c_float_scalar_class(element_type)}
                   : (nyr_inst_t){.op = NYIR_CONST_I64, .dst = -1, .imm = 1});
        nyr_op_t update_op =
            c_type_is_float(element_type)
                ? (element_type == C_F32
                       ? (decrement ? NYIR_SUB_F32 : NYIR_ADD_F32)
                       : (decrement ? NYIR_SUB_F64 : NYIR_ADD_F64))
                : (decrement ? NYIR_SUB_I64 : NYIR_ADD_I64);
        int updated =
            one < 0
                ? -1
                : emit_value(p, (nyr_inst_t){
                                    .op = update_op,
                                    .dst = -1,
                                    .a = loaded,
                                    .b = one,
                                    .scalar_class =
                                        c_type_is_float(element_type)
                                            ? c_float_scalar_class(element_type)
                                            : NYIR_SCALAR_I64_RAW});
        if (c_type_is_integer(element_type) && updated >= 0)
          updated = normalize_integer(p, element_type, updated);
        if (updated < 0 ||
            !emit(p,
                  (nyr_inst_t){
                      .op = NYIR_STORE_I64,
                      .dst = -1,
                      .a = address,
                      .c = updated,
                      .effects =
                          NYIR_EFFECT_WRITE_MEMORY | NYIR_EFFECT_MAY_TRAP |
                          (local->pointee_volatile ? NYIR_EFFECT_VOLATILE : 0),
                      .flags = memory_flags,
                      .scalar_class = c_type_is_float(element_type)
                                          ? c_float_scalar_class(element_type)
                                          : NYIR_SCALAR_I64_RAW},
                  NULL))
          return (c_value_t){-1, C_BAD, NULL};
      }
      return (c_value_t){loaded, element_type, NULL};
    }
    if (accept(p, "++") || accept(p, "--")) {
      ny_ctok_t op = p->tokens[p->pos - 1];
      bool decrement = ny_ctok_eq(op, "--");
      p->last_value_const = false;
      return (c_value_t){lower_increment(p, local, decrement, false, value),
                         local->type, local->agg};
    }
    if (local->type == C_AGG || local->type == C_AGG_PTR) {
      c_value_t base = {value, local->type, local->agg};
      c_value_t member;
      if (!lower_member_rvalue(p, base, &member))
        return (c_value_t){-1, C_BAD, NULL};
      if (array_shape_for_value(p, member.value))
        return member;
      return lower_pointer_index_rvalue(p, member);
    }
    if (local->type == C_FUNC_PTR) {
      p->indirect_bool_params = 0; /* set below only for sources that know it */
      p->indirect_result_type = local->function_result_type;
      p->indirect_result_agg = local->agg;
      p->has_indirect_result_signature = local->function_result_type != C_BAD;
      p->indirect_bool_params = local->function_bool_params;
      p->last_type_function_result_type = local->function_result_type;
      p->last_type_function_result_agg = local->agg;
      p->last_type_function_type = true;
    }
    return (c_value_t){value, local->type,
                       local->agg ? local->agg
                                  : c_pointee_marker(local->pointee_const,
                                                     local->pointee_volatile,
                                                     local->pointee_seg)};
  }
  /* NYC kernel compat: skip C99 designated initializers when encountered in
   * expression context */
  if (getenv("NYC_KERNEL_COMPAT") &&
      (tok_is(p, p->pos, "[") || tok_is(p, p->pos, "."))) {
    if (tok_is(p, p->pos, "[")) {
      int depth = 1;
      ++p->pos;
      while (p->pos < p->token_count && depth > 0) {
        if (tok_is(p, p->pos, "["))
          depth++;
        else if (tok_is(p, p->pos, "]"))
          depth--;
        if (depth > 0)
          ++p->pos;
      }
      if (depth == 0)
        ++p->pos; /* skip ] */
      (void)accept(p, "=");
      return parse_expr(p, 1);
    } else if (tok_is(p, p->pos, ".") && p->pos + 2 < p->token_count &&
               p->tokens[p->pos + 1].kind == NY_CTOK_IDENT &&
               tok_is(p, p->pos + 2, "=")) {
      p->pos += 3;
      return parse_expr(p, 1);
    }
  }
  char detail[128];
  snprintf(detail, sizeof(detail), "expected scalar expression near `%.64s`",
           tok.start ? tok.start : "");
  fail(p, detail);
  return (c_value_t){-1, C_BAD, NULL};
}

static c_value_t parse_postfix(c_parser_t *p) {
  c_value_t value = parse_primary(p);
  while (value.value >= 0) {
    if (tok_is(p, p->pos, "++") || tok_is(p, p->pos, "--")) {
      bool decrement = tok_is(p, p->pos, "--");
      const c_lvalue_t *lvalue = find_lvalue(p, value.value);
      c_local_t *local = lvalue && lvalue->is_local
                             ? find_local_slot(p, (size_t)lvalue->local_slot)
                             : NULL;
      if (!lvalue || (lvalue->is_local && !local)) {
        fail(p, "postfix increment and decrement require a modifiable lvalue");
        return (c_value_t){-1, C_BAD, NULL};
      }
      ++p->pos;
      int result =
          local ? lower_increment(p, local, decrement, false, value.value)
                : lower_increment_lvalue(p, lvalue, decrement, false,
                                         value.value);
      if (result < 0)
        return (c_value_t){-1, C_BAD, NULL};
      value = (c_value_t){result, lvalue->type, local ? local->agg : value.agg};
    } else if (value.type == C_FUNC_PTR && accept(p, "(")) {
      value = parse_indirect_call(p, value);
    } else if (tok_is(p, p->pos, "(")) {
      fail(p, "call target has unsupported C type");
      return (c_value_t){-1, C_BAD, NULL};
    } else if (array_shape_for_value(p, value.value) &&
               tok_is(p, p->pos, "[")) {
      value = lower_array_shape_index(p, value);
    } else if (tok_is(p, p->pos, "[")) {
      if (!c_type_is_pointer(value.type) || value.type == C_FUNC_PTR) {
        fail(p, "subscript requires a pointer to a complete object type");
        return (c_value_t){-1, C_BAD, NULL};
      }
      value = lower_pointer_index_rvalue(p, value);
    } else if (tok_is(p, p->pos, ".") || tok_is(p, p->pos, "->")) {
      c_value_t member;
      if (!lower_member_rvalue(p, value, &member))
        return (c_value_t){-1, C_BAD, NULL};
      value = member;
    } else {
      break;
    }
  }
  return value;
}

static c_value_t parse_unary(c_parser_t *p) {
  /*
   * System complex.h expands I to `(__extension__ 1.0iF)`, so the GNU
   * extension marker may lead an expression.
   */
  if (accept(p, "__extension__"))
    return parse_unary(p);
  if (accept(p, "_Alignof") || accept(p, "__alignof__") ||
      accept(p, "__alignof")) {
    c_parser_t probe = *p;
    char probe_error[1] = {0};
    probe.err = probe_error;
    probe.err_len = sizeof(probe_error);
    probe.type_lookahead = true;
    --probe.pos;
    uint64_t alignment = 0;
    if (!parse_array_extent_atom(&probe, &alignment)) {
      probe = *p;
      probe.err = probe_error;
      probe.err_len = sizeof(probe_error);
      probe.type_lookahead = true;
      c_type_t operand_type = C_BAD;
      if (!static_typeof_expr_type(&probe, &operand_type, 0)) {
        fail(p, "unsupported __alignof__ operand");
        return (c_value_t){-1, C_BAD, NULL};
      }
      alignment = probe.last_type_alignment;
      if (operand_type == C_AGG && probe.last_type_agg) {
        if (probe.last_type_agg->align > alignment)
          alignment = probe.last_type_agg->align;
      } else if (!alignment) {
        alignment = c_type_alignment(p->abi, operand_type);
      }
      if (!alignment) {
        fail(p, "unsupported __alignof__ operand");
        return (c_value_t){-1, C_BAD, NULL};
      }
      p->pos = probe.pos;
    }
    if (alignment > INT64_MAX) {
      fail(p, "unsupported __alignof__ operand");
      return (c_value_t){-1, C_BAD, NULL};
    }
    if (p->pos != probe.pos)
      p->pos = probe.pos;
    int value = emit_i64_constant(p, (int64_t)alignment);
    return value < 0 ? (c_value_t){-1, C_BAD, NULL}
                     : (c_value_t){value, c_size_type(p->abi), NULL};
  }
  if (accept(p, "sizeof")) {
    bool parenthesized = accept(p, "(");
    c_parser_t probe = *p;
    char probe_error[1] = {0};
    probe.err = probe_error;
    probe.err_len = sizeof(probe_error);
    probe.type_lookahead = true;
    bool anonymous_aggregate =
        parenthesized &&
        (tok_is(p, p->pos, "struct") || tok_is(p, p->pos, "union")) &&
        tok_is(p, p->pos + 1, "{");
    c_agg_type_t **scratch_tags = NULL;
    size_t scratch_tag_count = p->tag_count;
    if (anonymous_aggregate) {
      size_t capacity = p->tag_count + 1;
      scratch_tags = malloc(capacity * sizeof(*scratch_tags));
      if (!scratch_tags)
        return (c_value_t){-1, C_BAD, NULL};
      if (p->tag_count)
        memcpy(scratch_tags, p->tags, p->tag_count * sizeof(*scratch_tags));
      probe.tags = scratch_tags;
      probe.tag_capacity = capacity;
      probe.type_lookahead = false;
    }
    c_type_t type;
    size_t size = 0;
    bool known_zero_size = false;
    bool parsed_type = false;
    if (parenthesized && tok_is(&probe, probe.pos, "char") &&
        tok_is(&probe, probe.pos + 1, "[")) {
      c_parser_t array_probe = probe;
      ++array_probe.pos;
      size_t array_size = 1;
      unsigned array_rank = 0;
      while (accept(&array_probe, "[")) {
        uint64_t extent = 0;
        if (array_rank == C_MAX_ARRAY_RANK ||
            !parse_array_extent_expr(&array_probe, 1, &extent) || !extent ||
            extent > SIZE_MAX / array_size || !expect(&array_probe, "]")) {
          array_size = 0;
          break;
        }
        array_size *= (size_t)extent;
        ++array_rank;
      }
      if (array_size && array_rank &&
          tok_is(&array_probe, array_probe.pos, ")")) {
        probe = array_probe;
        type = C_CHAR;
        probe.last_type_is_array = true;
        probe.last_type_array_decayed = false;
        probe.last_type_array_size = array_size;
        parsed_type = true;
      }
    }
    if (!parsed_type)
      parsed_type = parse_type(&probe, &type, false);
    while (parsed_type && tok_is_attribute(&probe, probe.pos))
      parsed_type = parse_function_type_attributes(&probe);
    size_t aggregate_size = probe.last_type_agg ? probe.last_type_agg->size : 0;
    size_t aggregate_alignment =
        probe.last_type_agg ? probe.last_type_agg->align : 0;
    size_t type_alignment = probe.last_type_alignment;
    if (scratch_tags) {
      for (size_t i = scratch_tag_count; i < probe.tag_count; ++i) {
        free(probe.tags[i]->promoted_fields);
        free(probe.tags[i]);
      }
      free(probe.tags);
    }
    if (parsed_type && parenthesized && tok_is(&probe, probe.pos, "[")) {
      size_t array_size =
          type == C_AGG ? aggregate_size : c_type_size(p->abi, type);
      bool valid_array = array_size != 0;
      while (valid_array && accept(&probe, "[")) {
        uint64_t extent = 0;
        if (!parse_array_extent_expr(&probe, 1, &extent) ||
            !expect(&probe, "]") || extent > SIZE_MAX / array_size) {
          valid_array = false;
          break;
        }
        array_size *= (size_t)extent;
      }
      if (valid_array && array_size && array_size <= INT64_MAX &&
          tok_is(&probe, probe.pos, ")"))
        size = array_size;
    }
    if (parsed_type && (!parenthesized || tok_is(&probe, probe.pos, ")"))) {
      if (probe.last_type_is_array && !probe.last_type_array_decayed) {
        size = probe.last_type_array_size;
      } else if (type == C_AGG && aggregate_size && aggregate_alignment) {
        size = align_up(aggregate_size, type_alignment > aggregate_alignment
                                            ? type_alignment
                                            : aggregate_alignment);
      } else if (type == C_AGG && aggregate_alignment) {
        /*
         * Linux's BUILD_BUG_ON_ZERO uses sizeof(struct {
         *   _Static_assert(...);
         * }); GCC gives an empty GNU aggregate size zero.
         */
        if (parenthesized)
          ++probe.pos;
        p->pos = probe.pos;
        return (c_value_t){
            emit_value(p, (nyr_inst_t){.op = NYIR_CONST_I64, .dst = -1}),
            c_size_type(p->abi), NULL};
      } else if (type == C_U8 || type == C_CHAR)
        size = 1;
      else if (type == C_I16 || type == C_U16)
        size = 2;
      else if (type == C_I32 || type == C_U32 || type == C_F32)
        size = 4;
      else if (type == C_I64 || type == C_U64 || type == C_F64 ||
               c_type_is_complex(type) || c_type_is_pointer(type))
        size = c_type_size(p->abi, type);
    }
    if (!size && parenthesized && tok_ident(*current(p))) {
      ny_ctok_t object_name = *current(p);
      c_local_t *object_local = find_local(p, object_name);
      c_global_t *object_global =
          object_local ? NULL : find_global(p, object_name);
      size_t close = p->pos + 1;
      bool indexed_array =
          (object_local && object_local->array_count && object_local->agg) ||
          (object_local && object_local->array_rank) ||
          (object_local && object_local->type == C_AGG_PTR &&
           object_local->agg) ||
          (object_local &&
           (object_local->type == C_CHAR_PTR ||
            object_local->type == C_I8_PTR || object_local->type == C_I16_PTR ||
            object_local->type == C_U16_PTR ||
            object_local->type == C_I32_PTR ||
            object_local->type == C_U32_PTR ||
            object_local->type == C_I64_PTR ||
            object_local->type == C_U64_PTR ||
            object_local->type == C_F32_PTR ||
            object_local->type == C_F64_PTR ||
            object_local->type == C_CHAR_PTR_PTR ||
            object_local->type == C_I8_PTR_PTR ||
            object_local->type == C_FUNC_PTR_PTR ||
            object_local->type == C_OPAQUE_PTR_PTR ||
            object_local->type == C_AGG_PTR_PTR)) ||
          (object_global && object_global->is_array);
      if (indexed_array && tok_is(p, close, "[")) {
        size_t bracket = skip_balanced(p, close, "[", "]");
        if (bracket < p->token_count && tok_is(p, bracket, ")")) {
          if (object_local && object_local->array_rank) {
            size = c_type_size(p->abi, object_local->type);
            for (unsigned dim = 1; dim < object_local->array_rank; ++dim)
              size *= object_local->array_extent[dim];
          } else if (object_local && (object_local->type == C_CHAR_PTR_PTR ||
                                      object_local->type == C_I8_PTR_PTR ||
                                      object_local->type == C_FUNC_PTR_PTR ||
                                      object_local->type == C_OPAQUE_PTR_PTR ||
                                      object_local->type == C_AGG_PTR_PTR)) {
            size = c_type_size(p->abi, C_VOID_PTR);
          } else if (object_local && object_local->type == C_AGG_PTR &&
                     object_local->agg && object_local->agg->complete) {
            size = object_local->agg->size;
          } else if (object_local && object_local->type == C_AGG &&
                     object_local->array_count && object_local->agg &&
                     object_local->agg->complete) {
            size = object_local->agg->size;
          } else if (object_local) {
            size = object_local->type == C_CHAR_PTR ||
                           object_local->type == C_I8_PTR
                       ? 1
                   : object_local->type == C_I16_PTR ||
                           object_local->type == C_U16_PTR
                       ? 2
                   : object_local->type == C_I32_PTR ||
                           object_local->type == C_U32_PTR ||
                           object_local->type == C_F32_PTR
                       ? 4
                       : 8;
          } else {
            size = object_global->type == C_AGG && object_global->agg
                       ? object_global->agg->size
                       : c_type_size(p->abi, object_global->type);
          }
          probe.pos = bracket;
        }
      } else if ((object_local || object_global) && tok_is(p, close, ")")) {
        c_type_t object_type =
            object_local ? object_local->type : object_global->type;
        c_agg_type_t *object_agg =
            object_local ? object_local->agg : object_global->agg;
        size = object_local && object_local->array_rank
                   ? c_type_size(p->abi, object_local->type)
               : object_type == C_AGG && object_agg           ? object_agg->size
               : object_type == C_U8                          ? 1
               : object_type == C_I16 || object_type == C_U16 ? 2
               : object_type == C_I32 || object_type == C_U32 ||
                       object_type == C_F32
                   ? 4
               : object_type == C_I64 || object_type == C_U64 ||
                       object_type == C_F64
                   ? 8
               : c_type_is_pointer(object_type)
                   ? c_type_size(p->abi, object_type)
                   : 0;
        if (object_local && object_local->array_rank) {
          for (unsigned dim = 0; dim < object_local->array_rank; ++dim)
            size *= object_local->array_extent[dim];
        } else if (object_local && object_local->array_count && object_agg)
          size *= object_local->array_count;
        else if (object_global && object_global->is_array)
          size = object_global->len;
        if (!size && object_global && object_global->is_array)
          known_zero_size = true;
        if (size || known_zero_size)
          probe.pos = p->pos + 1;
      } else if ((object_local || object_global) &&
                 ((object_local ? object_local->type : object_global->type) ==
                      C_AGG ||
                  (object_local ? object_local->type : object_global->type) ==
                      C_AGG_PTR)) {
        c_agg_type_t *agg =
            object_local ? object_local->agg : object_global->agg;
        size_t at = close;
        size_t member_size = 0;
        while (agg && agg->complete &&
               (tok_is(p, at, ".") || tok_is(p, at, "->"))) {
          probe.pos = at;
          bool arrow = accept(&probe, "->");
          if (!arrow)
            (void)accept(&probe, ".");
          at = probe.pos;
          if (at >= p->token_count || !tok_ident(p->tokens[at]))
            break;
          const c_agg_field_t *field = find_field(agg, p->tokens[at++]);
          if (!field)
            break;
          if (field->kind == C_AGG_FIELD_AGG) {
            agg = (c_agg_type_t *)field->agg;
            member_size = agg && agg->complete ? agg->size : 0;
            probe.pos = at;
            continue;
          }
          member_size = field->size;
          if (tok_is(p, at, "[")) {
            size_t bracket = skip_balanced(p, at, "[", "]");
            if (bracket >= p->token_count)
              break;
            member_size =
                field->is_array ? field->elem_size
                : field->elem_pointer || field->elem_function_pointer ||
                        field->is_function_pointer
                    ? 8
                : field->agg && field->agg->complete ? field->agg->size
                : field->kind == C_AGG_FIELD_ARRAY ||
                        field->kind == C_AGG_FIELD_PTR
                    ? field->elem_size
                    : 0;
            if (!member_size)
              break;
            at = bracket;
            probe.pos = at;
          } else {
            probe.pos = at;
          }
          agg = NULL;
        }
        if (member_size && tok_is(p, at, ")")) {
          size = member_size;
          probe.pos = at;
        }
      }
    }
    if (!size && !known_zero_size && parenthesized && tok_is(p, p->pos, "(")) {
      size_t object_pos = p->pos;
      while (tok_is(p, object_pos, "("))
        ++object_pos;
      if (object_pos > p->pos && object_pos < p->token_count &&
          tok_ident(p->tokens[object_pos])) {
        ny_ctok_t object_name = p->tokens[object_pos];
        c_local_t *object_local = find_local(p, object_name);
        c_global_t *object_global =
            object_local ? NULL : find_global(p, object_name);
        size_t close = object_pos + 1;
        if ((object_local || object_global) && tok_is(p, close, ")")) {
          c_type_t object_type =
              object_local ? object_local->type : object_global->type;
          c_agg_type_t *object_agg =
              object_local ? object_local->agg : object_global->agg;
          size_t object_element_size = object_type == C_AGG && object_agg
                                           ? object_agg->size
                                           : c_type_size(p->abi, object_type);
          size = object_local &&
                         (object_local->array_rank || object_local->array_count)
                     ? object_local->array_count &&
                               object_element_size <=
                                   SIZE_MAX / object_local->array_count
                           ? object_element_size * object_local->array_count
                           : 0
                 : object_global && object_global->is_array ? object_global->len
                 : object_type == C_AGG && object_agg       ? object_agg->size
                 : object_type == C_U8 || object_type == C_CHAR ? 1
                 : object_type == C_I16 || object_type == C_U16 ? 2
                 : object_type == C_I32 || object_type == C_U32 ||
                         object_type == C_F32
                     ? 4
                 : object_type == C_I64 || object_type == C_U64 ||
                         object_type == C_F64
                     ? 8
                 : c_type_is_complex(object_type)
                     ? c_type_size(p->abi, object_type)
                 : c_type_is_pointer(object_type)
                     ? c_type_size(p->abi, object_type)
                     : 0;
          size_t nested = object_pos - p->pos;
          for (size_t i = 0; size && i <= nested; ++i)
            if (!tok_is(p, close + i, ")"))
              size = 0;
          if (size)
            probe.pos = close + nested;
        }
      }
    }
    if (!size && !known_zero_size && parenthesized &&
        p->pos + 1 < p->token_count && tok_is(p, p->pos, "*") &&
        tok_ident(p->tokens[p->pos + 1])) {
      ny_ctok_t object_name = p->tokens[p->pos + 1];
      c_local_t *object_local = find_local(p, object_name);
      size_t close = p->pos + 2;
      if (object_local && tok_is(p, close, ")")) {
        c_type_t object_type = object_local->type;
        c_agg_type_t *object_agg = complete_tag(p, object_local->agg);
        size = object_type == C_AGG_PTR && object_agg && object_agg->complete
                   ? object_agg->size
               : object_type == C_CHAR_PTR || object_type == C_I8_PTR ? 1
               : object_type == C_I16_PTR || object_type == C_U16_PTR ? 2
               : object_type == C_I32_PTR || object_type == C_U32_PTR ||
                       object_type == C_F32_PTR
                   ? 4
               : object_type == C_I64_PTR || object_type == C_U64_PTR ||
                       object_type == C_F64_PTR
                   ? 8
               : object_type == C_C64_PTR  ? 8
               : object_type == C_C128_PTR ? 16
               : c_type_is_pointer(object_type) && object_type != C_AGG_PTR
                   ? c_type_size(p->abi, object_type)
                   : 0;
        if (size)
          probe.pos = close;
      }
      if (!size && !known_zero_size && object_local &&
          (object_local->type == C_AGG_PTR || object_local->type == C_AGG) &&
          object_local->agg) {
        c_agg_type_t *agg = complete_tag(p, object_local->agg);
        size_t at = close;
        size_t pointee_size = 0;
        while (agg && agg->complete &&
               (tok_is(p, at, "->") || tok_is(p, at, "."))) {
          at++;
          if (at >= p->token_count || !tok_ident(p->tokens[at]))
            break;
          const c_agg_field_t *field = find_field(agg, p->tokens[at++]);
          if (!field)
            break;
          if (field->kind == C_AGG_FIELD_AGG) {
            agg = (c_agg_type_t *)field->agg;
            pointee_size = agg && agg->complete ? agg->size : 0;
            continue;
          }
          agg = NULL;
          c_agg_type_t *pointee = complete_tag(p, (c_agg_type_t *)field->agg);
          pointee_size =
              field->kind == C_AGG_FIELD_PTR && pointee && pointee->complete
                  ? pointee->size
              : field->kind == C_AGG_FIELD_PTR ? field->elem_size
                                               : field->size;
        }
        if (pointee_size && tok_is(p, at, ")")) {
          size = pointee_size;
          probe.pos = at;
        }
      }
    }
    if (!size && !known_zero_size && parenthesized) {
      c_parser_t void_deref_probe = *p;
      nyr_func_t void_deref_out;
      expression_probe_begin(&void_deref_probe, &void_deref_out);
      void_deref_probe.type_lookahead = true;
      if (accept(&void_deref_probe, "*")) {
        c_value_t pointer = parse_expr(&void_deref_probe, 14);
        if (pointer.type == C_VOID_PTR &&
            tok_is(&void_deref_probe, void_deref_probe.pos, ")")) {
          /*
           * GCC's sizeof(void) extension is used by Linux's __is_constexpr
           * type probe. The operand is unevaluated; model its size directly
           * rather than lowering a dereference of an incomplete void type.
           */
          size = 1;
          probe.pos = void_deref_probe.pos;
        }
      }
      expression_probe_end(&void_deref_probe);
    }
    if (!size && !known_zero_size && parenthesized) {
      c_parser_t expression_probe = *p;
      char expression_error[1] = {0};
      expression_probe.err = expression_error;
      expression_probe.err_len = sizeof(expression_error);
      expression_probe.type_lookahead = true;
      c_type_t expression_type = C_BAD;
      bool expression_type_ok =
          static_typeof_expr_type(&expression_probe, &expression_type, 0);
      if (expression_type_ok &&
          tok_is(&expression_probe, expression_probe.pos, ")")) {
        c_agg_type_t *expression_agg =
            complete_tag(p, (c_agg_type_t *)expression_probe.last_type_agg);
        if (expression_type == C_AGG && expression_agg &&
            expression_agg->complete) {
          size = expression_agg->size;
          known_zero_size = size == 0;
          probe.pos = expression_probe.pos;
        } else {
          size = expression_probe.last_type_is_array
                     ? expression_probe.last_type_array_size
                     : c_type_size(p->abi, expression_type);
          known_zero_size = expression_probe.last_type_is_array && size == 0;
        }
        if (size || known_zero_size)
          probe.pos = expression_probe.pos;
      }
      if (!size && !known_zero_size) {
        expression_probe = *p;
        expression_probe.err = expression_error;
        expression_probe.err_len = sizeof(expression_error);
        expression_probe.type_lookahead = false;
        nyr_func_t expression_out;
        expression_probe_begin(&expression_probe, &expression_out);
        c_value_t expression = parse_expr(&expression_probe, 0);
        if (expression.value >= 0 &&
            tok_is(&expression_probe, expression_probe.pos, ")")) {
          c_agg_type_t *expression_agg =
              complete_tag(p, (c_agg_type_t *)expression.agg);
          if (expression.type == C_AGG && expression_agg &&
              expression_agg->complete) {
            size = expression_agg->size;
            known_zero_size = true;
            probe.pos = expression_probe.pos;
          } else {
            size = c_type_size(p->abi, expression.type);
          }
          if (size && !known_zero_size)
            probe.pos = expression_probe.pos;
        }
        expression_probe_end(&expression_probe);
      }
    }
    if (!size && !known_zero_size && !parenthesized) {
      c_parser_t expression_probe = *p;
      char expression_error[1] = {0};
      expression_probe.err = expression_error;
      expression_probe.err_len = sizeof(expression_error);
      expression_probe.type_lookahead = true;
      nyr_func_t expression_out;
      expression_probe_begin(&expression_probe, &expression_out);
      c_value_t expression = parse_unary(&expression_probe);
      if (expression.value >= 0) {
        c_agg_type_t *expression_agg =
            complete_tag(p, (c_agg_type_t *)expression.agg);
        size = expression_probe.last_type_is_array
                   ? expression_probe.last_type_array_size
               : expression.type == C_AGG && expression_agg &&
                       expression_agg->complete
                   ? expression_agg->size
                   : c_type_size(p->abi, expression.type);
        if (size)
          probe.pos = expression_probe.pos;
      }
      expression_probe_end(&expression_probe);
    }
    if (!size && !known_zero_size) {
      fail(p, "sizeof requires a supported complete C type");
      return (c_value_t){-1, C_BAD, NULL};
    }
    if (parenthesized)
      ++probe.pos;
    p->pos = probe.pos;
    return (c_value_t){emit_value(p, (nyr_inst_t){.op = NYIR_CONST_I64,
                                                  .dst = -1,
                                                  .imm = (int64_t)size}),
                       c_size_type(p->abi), NULL};
  }
  if (accept(p, "++") || accept(p, "--")) {
    bool decrement = ny_ctok_eq(p->tokens[p->pos - 1], "--");
    size_t operand_start = p->pos;
    c_local_t *local = NULL;
    if (tok_ident(*current(p))) {
      ny_ctok_t name;
      if (!parse_name(p, &name))
        return (c_value_t){-1, C_BAD, NULL};
      local = find_local(p, name);
    }
    if (local && !tok_is(p, p->pos, ".") && !tok_is(p, p->pos, "->") &&
        !tok_is(p, p->pos, "[")) {
      int value = lower_increment(p, local, decrement, true, -1);
      p->last_value_const = false;
      return (c_value_t){value, local->type, local->agg};
    }
    p->pos = operand_start;
    c_value_t operand = parse_unary(p);
    const c_lvalue_t *lvalue =
        operand.value >= 0 ? find_lvalue(p, operand.value) : NULL;
    c_local_t *target_local =
        lvalue && lvalue->is_local
            ? find_local_slot(p, (size_t)lvalue->local_slot)
            : NULL;
    int value =
        target_local
            ? lower_increment(p, target_local, decrement, true, -1)
            : lower_increment_lvalue(p, lvalue, decrement, true, operand.value);
    p->last_value_const = false;
    return (c_value_t){value, value >= 0 ? operand.type : C_BAD, operand.agg};
  }
  if (accept(p, "&&")) {
    ny_ctok_t label_name = {0};
    c_label_t *label = NULL;
    if (!p->function || !parse_name(p, &label_name) ||
        !(label = find_label_at(p, label_name, p->pos))) {
      fail(p, "GNU label address must name a label in the current function");
      return (c_value_t){-1, C_BAD, NULL};
    }
    label->address_taken = true;
    p->function->noinline = true;
    char symbol_name[64];
    int length = snprintf(symbol_name, sizeof(symbol_name),
                          "__nyc_local_label_%zu", label->id);
    if (length < 0 || (size_t)length >= sizeof(symbol_name)) {
      fail(p, "GNU label address identifier is too large");
      return (c_value_t){-1, C_BAD, NULL};
    }
    const char *symbol = nyr_func_own_symbol_copy(p->out, symbol_name);
    if (!symbol) {
      fail(p, "out of memory lowering GNU label address");
      return (c_value_t){-1, C_BAD, NULL};
    }
    int value =
        emit_value(p, (nyr_inst_t){.op = NYIR_ADDR_SYMBOL,
                                   .dst = -1,
                                   .symbol = symbol,
                                   .scalar_class = NYIR_SCALAR_I64_RAW});
    return value < 0 ? (c_value_t){-1, C_BAD, NULL}
                     : (c_value_t){value, C_VOID_PTR, NULL};
  }
  if (accept(p, "&")) {
    size_t address_start = p->pos - 1;
    if (tok_is(p, p->pos, "(") && tok_ident(p->tokens[p->pos + 1]) &&
        tok_is(p, p->pos + 2, ")") && tok_is(p, p->pos + 3, "[")) {
      c_value_t element = parse_postfix(p);
      const c_lvalue_t *lvalue =
          element.value >= 0 ? find_lvalue(p, element.value) : NULL;
      if (!lvalue || lvalue->bitfield ||
          (lvalue->type != C_AGG && !discard_lvalue_read(p, lvalue))) {
        fail(p, "address-of parenthesized array subscript requires an lvalue");
        return (c_value_t){-1, C_BAD, NULL};
      }
      c_type_t pointer_type = c_add_pointer_levels(lvalue->type, 1);
      if (pointer_type == C_BAD)
        return (c_value_t){-1, C_BAD, NULL};
      p->last_value_null_pointer_constant = false;
      return (c_value_t){lvalue->address, pointer_type, lvalue->agg};
    }
    bool has_chained_subscript = false;
    size_t subscript = p->pos + 1;
    while ((tok_is(p, subscript, ".") || tok_is(p, subscript, "->")) &&
           tok_ident(p->tokens[subscript + 1]))
      subscript += 2;
    if (tok_is(p, subscript, "[")) {
      size_t close = skip_balanced(p, subscript, "[", "]");
      has_chained_subscript = close < p->token_count && tok_is(p, close, "[");
    }
    c_global_t *indexed_global =
        tok_ident(*current(p)) ? find_global(p, *current(p)) : NULL;
    if (indexed_global && indexed_global->is_pointer_array &&
        has_chained_subscript) {
      c_value_t element = parse_postfix(p);
      const c_lvalue_t *lvalue =
          element.value >= 0 ? find_lvalue(p, element.value) : NULL;
      if (!lvalue || lvalue->bitfield ||
          (lvalue->type != C_AGG && !discard_lvalue_read(p, lvalue))) {
        fail(p, "address-of indexed expression requires a non-bitfield lvalue");
        return (c_value_t){-1, C_BAD, NULL};
      }
      c_type_t pointer_type = c_add_pointer_levels(lvalue->type, 1);
      if (pointer_type == C_BAD)
        return (c_value_t){-1, C_BAD, NULL};
      p->last_value_null_pointer_constant = false;
      return (c_value_t){lvalue->address, pointer_type, lvalue->agg};
    }
    if (tok_is(p, p->pos, "(")) {
      c_parser_t compound_probe = *p;
      char compound_error[1] = {0};
      compound_probe.err = compound_error;
      compound_probe.err_len = sizeof(compound_error);
      compound_probe.type_lookahead = true;
      c_type_t compound_type = C_BAD;
      if (accept(&compound_probe, "(") &&
          parse_type(&compound_probe, &compound_type, false) &&
          compound_type != C_AGG && expect(&compound_probe, ")") &&
          tok_is(&compound_probe, compound_probe.pos, "{")) {
        size_t width = c_type_size(p->abi, compound_type);
        int address =
            emit_value(p, (nyr_inst_t){.op = NYIR_ALLOCA,
                                       .dst = -1,
                                       .imm = (int64_t)width,
                                       .scalar_class = NYIR_SCALAR_I64_RAW});
        if (!width || address < 0)
          return (c_value_t){-1, C_BAD, NULL};
        p->pos = compound_probe.pos + 1;
        c_value_t initializer = parse_expr(p, 1);
        if (initializer.value < 0 || !expect(p, "}") ||
            (initializer.type != compound_type &&
             !(c_type_is_integer(initializer.type) &&
               c_type_is_integer(compound_type)))) {
          if (!p->err[0])
            fail(p,
                 "scalar compound literal requires a compatible initializer");
          return (c_value_t){-1, C_BAD, NULL};
        }
        if (c_type_is_integer(compound_type)) {
          initializer.value =
              normalize_integer(p, compound_type, initializer.value);
          if (initializer.value < 0)
            return (c_value_t){-1, C_BAD, NULL};
        }
        uint8_t flags = c_local_memory_flags(p, compound_type);
        if (!emit(p,
                  (nyr_inst_t){.op = NYIR_STORE_I64,
                               .dst = -1,
                               .a = address,
                               .c = initializer.value,
                               .effects = NYIR_EFFECT_WRITE_MEMORY |
                                          NYIR_EFFECT_MAY_TRAP,
                               .flags = flags,
                               .scalar_class = NYIR_SCALAR_I64_RAW},
                  NULL))
          return (c_value_t){-1, C_BAD, NULL};
        c_type_t pointer_type = c_add_pointer_levels(compound_type, 1);
        p->last_value_null_pointer_constant = false;
        return pointer_type == C_BAD ? (c_value_t){-1, C_BAD, NULL}
                                     : (c_value_t){address, pointer_type, NULL};
      }
    }
    if (tok_ident(*current(p)) && !find_local(p, *current(p)) &&
        !tok_is(p, p->pos + 1, "(") && find_function(p, *current(p))) {
      c_function_t *signature = find_function(p, *current(p));
      if (signature->has_asm_alias && !signature->asm_alias.len) {
        fail(p, "function asm label must be a string literal");
        return (c_value_t){-1, C_BAD, NULL};
      }
      static const char extern_prefix[] = "__nyc_extern_";
      ny_ctok_t link_name =
          signature->asm_alias.len ? signature->asm_alias : signature->name;
      size_t prefix_len = signature->asm_alias.len || signature->defined
                              ? 0
                              : sizeof(extern_prefix) - 1;
      if (link_name.len > SIZE_MAX - prefix_len - 1)
        return (c_value_t){-1, C_BAD, NULL};
      char *symbol = malloc(prefix_len + link_name.len + 1);
      if (!symbol)
        return (c_value_t){-1, C_BAD, NULL};
      if (prefix_len)
        memcpy(symbol, extern_prefix, prefix_len);
      memcpy(symbol + prefix_len, link_name.start, link_name.len);
      symbol[prefix_len + link_name.len] = '\0';
      int address = emit_value(
          p, (nyr_inst_t){.op = NYIR_ADDR_SYMBOL, .dst = -1, .symbol = symbol});
      free(symbol);
      if (address < 0)
        return (c_value_t){-1, C_BAD, NULL};
      ++p->pos;
      p->indirect_bool_params = 0; /* set below only for sources that know it */
      p->indirect_result_type = signature->result;
      p->indirect_result_agg = signature->result_agg;
      p->has_indirect_result_signature = true;
      p->last_value_null_pointer_constant = false;
      return (c_value_t){address, C_FUNC_PTR, NULL};
    }
    if (p->pos + 3 < p->token_count && tok_is(p, p->pos, "(") &&
        tok_ident(p->tokens[p->pos + 1]) && tok_is(p, p->pos + 2, ")") &&
        tok_is(p, p->pos + 3, "[")) {
      c_global_t *global = find_global(p, p->tokens[p->pos + 1]);
      if (global && global->is_pointer_array) {
        ++p->pos;
        c_value_t base = parse_primary(p);
        if (base.value < 0 || !expect(p, ")") || !expect(p, "[")) {
          fail(p, "invalid parenthesized pointer-array address");
          return (c_value_t){-1, C_BAD, NULL};
        }
        c_value_t index = parse_expr(p, 1);
        if (index.value < 0 || !c_type_is_integer(index.type) ||
            !expect(p, "]")) {
          fail(p, "pointer-array address index must be an integer");
          return (c_value_t){-1, C_BAD, NULL};
        }
        int address = lower_pointer_index_address(p, C_OPAQUE_PTR, base.value,
                                                  index.value);
        int value =
            address < 0
                ? -1
                : emit_value(p,
                             (nyr_inst_t){.op = NYIR_LOAD_I64,
                                          .dst = -1,
                                          .a = address,
                                          .effects = NYIR_EFFECT_READ_MEMORY |
                                                     NYIR_EFFECT_MAY_TRAP,
                                          .scalar_class = NYIR_SCALAR_I64_RAW});
        c_type_t pointer_type = c_type_is_pointer(global->type)
                                    ? global->type
                                    : c_add_pointer_levels(global->type, 1);
        if (value < 0 || pointer_type == C_BAD ||
            !record_lvalue(p, value, address, global->type,
                           global->is_volatile)) {
          fail(p, "could not form a pointer-array element address");
          return (c_value_t){-1, C_BAD, NULL};
        }
        p->last_value_null_pointer_constant = false;
        return (c_value_t){address, pointer_type, global->agg};
      }
    }
    if (tok_ident(*current(p)) && tok_is(p, p->pos + 1, "[")) {
      c_global_t *global = find_global(p, *current(p));
      size_t subscript_end = skip_balanced(p, p->pos + 1, "[", "]");
      bool indexed_member =
          subscript_end < p->token_count &&
          (tok_is(p, subscript_end, ".") || tok_is(p, subscript_end, "->"));
      if (global && (global->is_pointer_array ||
                     (c_type_is_pointer(global->type) && indexed_member))) {
        if (indexed_member) {
          c_value_t member = parse_postfix(p);
          const c_lvalue_t *lvalue =
              member.value >= 0 ? find_lvalue(p, member.value) : NULL;
          c_type_t pointer_type = c_add_pointer_levels(member.type, 1);
          if (!lvalue || lvalue->bitfield || pointer_type == C_BAD ||
              (lvalue->type != C_AGG && !discard_lvalue_read(p, lvalue))) {
            fail(p, "address-of indexed pointer-array member requires a "
                    "non-bitfield lvalue");
            return (c_value_t){-1, C_BAD, NULL};
          }
          p->last_value_null_pointer_constant = false;
          return (c_value_t){lvalue->address, pointer_type, member.agg};
        }
        c_value_t element = parse_primary(p);
        const c_lvalue_t *lvalue =
            element.value >= 0 ? find_lvalue(p, element.value) : NULL;
        c_type_t pointer_type = c_add_pointer_levels(element.type, 1);
        if (!lvalue && global->array_rank > 1 &&
            element.type == c_array_pointer_type(global->type) &&
            pointer_type != C_BAD) {
          p->last_value_null_pointer_constant = false;
          return (c_value_t){element.value, pointer_type, element.agg};
        }
        if (!lvalue || pointer_type == C_BAD) {
          fail(p, "address-of global pointer-array element requires an lvalue");
          return (c_value_t){-1, C_BAD, NULL};
        }
        p->last_value_null_pointer_constant = false;
        return (c_value_t){lvalue->address, pointer_type, element.agg};
      }
    }
    if (tok_ident(*current(p)) &&
        (tok_is(p, p->pos + 1, ".") || tok_is(p, p->pos + 1, "->"))) {
      c_value_t operand = parse_postfix(p);
      if (operand.value < 0)
        return operand;
      if (operand.type == C_AGG) {
        p->last_value_null_pointer_constant = false;
        return (c_value_t){operand.value, C_AGG_PTR, operand.agg};
      }
      if (p->last_value_array && c_type_is_pointer(operand.type)) {
        p->last_value_null_pointer_constant = false;
        if (operand.type == C_AGG_PTR)
          return (c_value_t){operand.value, C_AGG_ARRAY_PTR, operand.agg};
        return operand;
      }
      const c_lvalue_t *lvalue = find_lvalue(p, operand.value);
      if (!lvalue || lvalue->bitfield) {
        fail(p, "address-of member requires a non-bitfield lvalue");
        return (c_value_t){-1, C_BAD, NULL};
      }
      if (!discard_lvalue_read(p, lvalue))
        return (c_value_t){-1, C_BAD, NULL};
      c_type_t pointer_type = c_add_pointer_levels(lvalue->type, 1);
      if (pointer_type == C_BAD)
        return (c_value_t){-1, C_BAD, NULL};
      p->last_value_null_pointer_constant = false;
      p->last_value_pointee_const = p->last_value_const;
      p->last_value_const = false;
      return (c_value_t){
          lvalue->address, pointer_type,
          lvalue->agg ? lvalue->agg
                      : c_pointee_marker(p->last_value_pointee_const, false,
                                         p->last_value_pointee_seg)};
    }
    if (tok_is(p, p->pos, "_Generic")) {
      c_value_t operand = parse_postfix(p);
      const c_lvalue_t *lvalue =
          operand.value >= 0 ? find_lvalue(p, operand.value) : NULL;
      if (!lvalue || lvalue->bitfield) {
        fail(p, "address-of _Generic member requires a non-bitfield lvalue");
        return (c_value_t){-1, C_BAD, NULL};
      }
      if (lvalue->type != C_AGG && !discard_lvalue_read(p, lvalue))
        return (c_value_t){-1, C_BAD, NULL};
      c_type_t pointer_type = c_add_pointer_levels(lvalue->type, 1);
      if (pointer_type == C_BAD)
        return (c_value_t){-1, C_BAD, NULL};
      p->last_value_null_pointer_constant = false;
      return (c_value_t){lvalue->address, pointer_type,
                         lvalue->agg ? lvalue->agg : operand.agg};
    }
    if (tok_is(p, p->pos, "(") && tok_is(p, p->pos + 1, "(")) {
      c_parser_t probe = *p;
      char probe_error[1] = {0};
      probe.err = probe_error;
      probe.err_len = sizeof(probe_error);
      probe.type_lookahead = true;
      c_type_t base_type = C_BAD;
      ny_ctok_t member = {0};
      if (expect(&probe, "(") && expect(&probe, "(") &&
          parse_type(&probe, &base_type, false) && base_type == C_AGG_PTR &&
          expect(&probe, ")") && tok_is(&probe, probe.pos, "0") &&
          expect(&probe, "0") && expect(&probe, ")") && expect(&probe, "->") &&
          parse_name(&probe, &member)) {
        c_agg_type_t *aggregate = complete_tag(p, probe.last_type_agg);
        const c_agg_field_t *field =
            aggregate ? find_field(aggregate, member) : NULL;
        if (field && field->offset <= INT64_MAX) {
          p->pos = probe.pos;
          return (c_value_t){emit_i64_constant(p, (int64_t)field->offset),
                             C_I64, NULL};
        }
      }
    }
    if (accept(p, "*")) {
      c_value_t pointer = parse_unary(p);
      if (pointer.value < 0 || !c_type_is_pointer(pointer.type)) {
        fail(p, "address-of a dereference requires a pointer operand");
        return (c_value_t){-1, C_BAD, NULL};
      }
      p->last_value_null_pointer_constant = false;
      return pointer;
    }
    if (tok_ident(*current(p)) && tok_is(p, p->pos + 1, "(") &&
        (find_function(p, *current(p)) ||
         tok_is(p, p->pos, "__builtin_choose_expr"))) {
      size_t call_end = skip_balanced(p, address_start + 2, "(", ")");
      bool has_member = call_end < p->token_count &&
                        (tok_is(p, call_end, ".") ||
                         tok_is(p, call_end, "->") || tok_is(p, call_end, "["));
      c_value_t operand = parse_postfix(p);
      if (operand.value >= 0 && operand.type == C_AGG_PTR) {
        const c_lvalue_t *lvalue = find_lvalue(p, operand.value);
        if (lvalue && lvalue->type == C_AGG_PTR) {
          c_type_t pointer_type = c_add_pointer_levels(lvalue->type, 1);
          if (pointer_type == C_BAD)
            return (c_value_t){-1, C_BAD, NULL};
          p->last_value_null_pointer_constant = false;
          return (c_value_t){lvalue->address, pointer_type, lvalue->agg};
        }
        p->last_value_null_pointer_constant = false;
        return operand;
      }
      if (operand.value >= 0 && operand.type == C_AGG && has_member) {
        p->last_value_null_pointer_constant = false;
        return (c_value_t){operand.value, C_AGG_PTR, operand.agg};
      }
      if (operand.value >= 0 && operand.type == C_AGG_ARRAY_PTR && has_member) {
        p->last_value_null_pointer_constant = false;
        return operand;
      }
      const c_lvalue_t *lvalue =
          operand.value >= 0 ? find_lvalue(p, operand.value) : NULL;
      if (has_member && c_type_is_pointer(operand.type) &&
          (!lvalue ||
           (lvalue->address >= 0 && operand.value == lvalue->address))) {
        p->last_value_null_pointer_constant = false;
        return operand;
      }
      if (!has_member || !lvalue || lvalue->bitfield || lvalue->address < 0 ||
          (lvalue->type != C_AGG && !discard_lvalue_read(p, lvalue))) {
        fail(p, "address-of function-call member requires an aggregate lvalue");
        return (c_value_t){-1, C_BAD, NULL};
      }
      c_type_t pointer_type = c_add_pointer_levels(lvalue->type, 1);
      if (pointer_type == C_BAD)
        return (c_value_t){-1, C_BAD, NULL};
      p->last_value_null_pointer_constant = false;
      return (c_value_t){lvalue->address, pointer_type,
                         lvalue->agg ? lvalue->agg : operand.agg};
    }
    if (current(p)->kind == NY_CTOK_STRING) {
      c_value_t string = lower_string_literal(p);
      if (string.value < 0 || string.type != C_CHAR_PTR || !accept(p, "[")) {
        fail(p, "address-of string literal requires a character subscript");
        return (c_value_t){-1, C_BAD, NULL};
      }
      c_value_t index = parse_expr(p, 1);
      if (index.value < 0 || !c_type_is_integer(index.type) ||
          !expect(p, "]")) {
        fail(p, "address-of string literal requires an integer index");
        return (c_value_t){-1, C_BAD, NULL};
      }
      int address =
          lower_pointer_index_address(p, C_CHAR_PTR, string.value, index.value);
      return address < 0 ? (c_value_t){-1, C_BAD, NULL}
                         : (c_value_t){address, C_CHAR_PTR, NULL};
    }
    size_t parenthesized_name_closes = 0;
    size_t name_pos = p->pos;
    while (tok_is(p, name_pos, "("))
      ++name_pos, ++parenthesized_name_closes;
    if (parenthesized_name_closes && name_pos < p->token_count &&
        tok_ident(p->tokens[name_pos])) {
      size_t close_pos = name_pos + 1;
      size_t closes = 0;
      while (tok_is(p, close_pos, ")"))
        ++close_pos, ++closes;
      size_t matched_close_pos = name_pos + 1 + parenthesized_name_closes;
      size_t subscript_end = tok_is(p, name_pos + 1, "[")
                                 ? skip_balanced(p, name_pos + 1, "[", "]")
                                 : name_pos + 1;
      bool parenthesized_subscript = tok_is(p, name_pos + 1, "[") &&
                                     subscript_end < p->token_count &&
                                     tok_is(p, subscript_end, ")") &&
                                     (tok_is(p, subscript_end + 1, ")") ||
                                      tok_is(p, subscript_end + 1, ",") ||
                                      tok_is(p, subscript_end + 1, ";"));
      if (parenthesized_subscript || (closes >= parenthesized_name_closes &&
                                      (tok_is(p, matched_close_pos, ")") ||
                                       tok_is(p, matched_close_pos, "[") ||
                                       tok_is(p, matched_close_pos, ",") ||
                                       tok_is(p, matched_close_pos, "=") ||
                                       tok_is(p, matched_close_pos, ";"))))
        p->pos = name_pos;
      else
        parenthesized_name_closes = 0;
    } else {
      parenthesized_name_closes = 0;
    }
    if (tok_is(p, p->pos, "(")) {
      c_value_t base = parse_unary(p);
      if (base.value < 0)
        return base;
      if ((base.type == C_AGG || base.type == C_AGG_PTR) &&
          (tok_is(p, p->pos, ".") || tok_is(p, p->pos, "->"))) {
        c_value_t address = lower_address_aggregate_members(p, base);
        if (address.value < 0)
          return (c_value_t){-1, C_BAD, NULL};
        (void)accept(p, ")");
        return address;
      }
      /*
       * An aggregate operand is addressed in place; a stale array flag
       * from the object it was derived from must not turn this into an
       * opaque pointer to the element.
       */
      if (base.type == C_AGG)
        return (c_value_t){base.value, C_AGG_PTR, base.agg};
      if (p->last_value_array)
        return (c_value_t){base.value, C_OPAQUE_PTR, base.agg};
      const c_lvalue_t *lvalue = find_lvalue(p, base.value);
      if (c_type_is_pointer(base.type) && address_start + 4 < p->token_count &&
          (tok_is(p, p->pos, ")") || tok_is(p, p->pos - 1, ")")) &&
          tok_ident(p->tokens[address_start + 2]) &&
          (tok_is(p, address_start + 3, ".") ||
           tok_is(p, address_start + 3, "->")) &&
          tok_ident(p->tokens[address_start + 4])) {
        c_agg_type_t *aggregate = NULL;
        c_local_t *owner = find_local(p, p->tokens[address_start + 2]);
        if (owner && (owner->type == C_AGG || owner->type == C_AGG_PTR))
          aggregate = owner->agg;
        c_global_t *global = find_global(p, p->tokens[address_start + 2]);
        if (!aggregate && global &&
            (global->type == C_AGG || global->type == C_AGG_PTR))
          aggregate = global->agg;
        const c_agg_field_t *field =
            aggregate ? find_field(aggregate, p->tokens[address_start + 4])
                      : NULL;
        if (field && field->kind == C_AGG_FIELD_ARRAY && field->elem_size == 1)
          return base;
      }
      if (lvalue) {
        if (lvalue->bitfield) {
          fail(p, "cannot take the address of a C bitfield");
          return (c_value_t){-1, C_BAD, NULL};
        }
        if (!discard_lvalue_read(p, lvalue)) {
          fail(p, "cannot isolate the final C lvalue read for address-of");
          return (c_value_t){-1, C_BAD, NULL};
        }
        int address = lvalue->address;
        if (lvalue->is_local) {
          c_local_t *local = find_local_slot(p, (size_t)lvalue->local_slot);
          if (!local)
            return (c_value_t){-1, C_BAD, NULL};
          address =
              local->volatile_address >= 0
                  ? local->volatile_address
                  : emit_value(
                        p, (nyr_inst_t){.op = NYIR_ADDR_LOCAL,
                                        .dst = -1,
                                        .imm = (int64_t)local->slot,
                                        .scalar_class = NYIR_SCALAR_I64_RAW});
        }
        c_type_t pointer_type = base.type == C_AGG_PTR    ? C_AGG_PTR_PTR
                                : base.type == C_CHAR_PTR ? C_CHAR_PTR_PTR
                                : base.type == C_U8       ? C_CHAR_PTR
                                : base.type == C_CHAR     ? C_I8_PTR
                                : base.type == C_FUNC_PTR ? C_FUNC_PTR_PTR
                                : base.type == C_I16      ? C_I16_PTR
                                : base.type == C_U16      ? C_U16_PTR
                                : base.type == C_I32      ? C_I32_PTR
                                : base.type == C_U32      ? C_U32_PTR
                                : base.type == C_I64      ? C_I64_PTR
                                : base.type == C_U64      ? C_U64_PTR
                                : base.type == C_F32      ? C_F32_PTR
                                : base.type == C_F64      ? C_F64_PTR
                                                          : C_OPAQUE_PTR;
        if (address < 0 || pointer_type == C_BAD)
          return (c_value_t){-1, C_BAD, NULL};
        return (c_value_t){address, pointer_type, base.agg};
      }
      if (!c_type_is_object_pointer(base.type) || base.type == C_VOID_PTR) {
        if (!getenv("NYC_KERNEL_COMPAT")) {
          char message[96];
          snprintf(
              message, sizeof(message),
              "parenthesized address-of requires a typed pointer subscript "
              "(type %d)",
              base.type);
          fail(p, message);
          return (c_value_t){-1, C_BAD, NULL};
        }
        /* NYC kernel compat: WRITE_ONCE/READ_ONCE and other macros produce
           parenthesized address-of forms NYC may mis-type; continue with
           opaque-pointer semantics. */
      }
      if (tok_is(p, p->pos, ")") && tok_is(p, p->pos + 1, "["))
        ++p->pos;
      size_t address_operand = address_start + 1;
      while (tok_is(p, address_operand, "("))
        ++address_operand;
      if (c_type_is_pointer(base.type) && tok_is(p, address_operand, "*")) {
        /*
         * &(*ptr) is exactly ptr; Linux per-CPU macros use this form.
         */
        return base;
      }
      if (!accept(p, "[")) {
        if (getenv("NYC_KERNEL_COMPAT")) {
          /* Try to treat as address-of the base lvalue if possible */
          const c_lvalue_t *lvalue = find_lvalue(p, base.value);
          if (lvalue && !lvalue->bitfield) {
            if (discard_lvalue_read(p, lvalue)) {
              int address = lvalue->address;
              if (lvalue->is_local) {
                c_local_t *local =
                    find_local_slot(p, (size_t)lvalue->local_slot);
                if (local) {
                  address =
                      local->volatile_address >= 0
                          ? local->volatile_address
                          : emit_value(p,
                                       (nyr_inst_t){.op = NYIR_ADDR_LOCAL,
                                                    .dst = -1,
                                                    .imm = (int64_t)local->slot,
                                                    .scalar_class =
                                                        NYIR_SCALAR_I64_RAW});
                }
              }
              if (address >= 0) {
                c_type_t pointer_type =
                    base.type == C_AGG_PTR    ? C_AGG_PTR_PTR
                    : base.type == C_CHAR_PTR ? C_CHAR_PTR_PTR
                    : base.type == C_U8       ? C_CHAR_PTR
                    : base.type == C_CHAR     ? C_I8_PTR
                    : base.type == C_FUNC_PTR ? C_FUNC_PTR_PTR
                    : base.type == C_I16      ? C_I16_PTR
                    : base.type == C_U16      ? C_U16_PTR
                    : base.type == C_I32      ? C_I32_PTR
                    : base.type == C_U32      ? C_U32_PTR
                    : base.type == C_I64      ? C_I64_PTR
                    : base.type == C_U64      ? C_U64_PTR
                    : base.type == C_F32      ? C_F32_PTR
                    : base.type == C_F64      ? C_F64_PTR
                                              : C_OPAQUE_PTR;
                if (pointer_type != C_BAD)
                  return (c_value_t){address, pointer_type, base.agg};
              }
            }
          }
          /* Fallback to opaque pointer */
          return (c_value_t){base.value, C_OPAQUE_PTR, base.agg};
        }
        char message[192];
        ny_ctok_t next = *current(p);
        size_t used = (size_t)snprintf(
            message, sizeof(message),
            "unsupported parenthesized address-of type %d near '%.*s'",
            base.type, (int)(next.len > 32 ? 32 : next.len), next.start);
        for (size_t i = address_start;
             i < p->token_count && i < p->pos + 3 && used < sizeof(message);
             ++i) {
          ny_ctok_t token = p->tokens[i];
          int written =
              snprintf(message + used, sizeof(message) - used, " %.*s",
                       (int)(token.len > 24 ? 24 : token.len), token.start);
          if (written < 0 || (size_t)written >= sizeof(message) - used)
            break;
          used += (size_t)written;
        }
        fail(p, message);
        return (c_value_t){-1, C_BAD, NULL};
      }
      c_value_t index = parse_expr(p, 1);
      if (index.value < 0 || !c_type_is_integer(index.type) ||
          !expect(p, "]")) {
        fail(p, "address-of pointer subscript requires an integer index");
        return (c_value_t){-1, C_BAD, NULL};
      }
      int address =
          lower_pointer_index_address(p, base.type, base.value, index.value);
      return address < 0 ? (c_value_t){-1, C_BAD, NULL}
                         : (c_value_t){address, base.type, base.agg};
    }
    ny_ctok_t name;
    if (!parse_name(p, &name))
      return (c_value_t){-1, C_BAD, NULL};
    if (!tok_is(p, p->pos, "["))
      for (size_t i = 0; i < parenthesized_name_closes; ++i)
        if (!expect(p, ")"))
          return (c_value_t){-1, C_BAD, NULL};
    c_local_t *local = find_local(p, name);
    if (local)
      local->has_integer_constant = false;
    if (local && c_type_is_float(local->type))
      local->address_taken = true;
    bool local_member =
        (local && local->type == C_AGG &&
         (tok_is(p, p->pos, ".") || tok_is(p, p->pos, "->"))) ||
        (local && local->type == C_AGG_PTR && tok_is(p, p->pos, "->"));
    if (local_member) {
      int base = lower_local_load(p, local);
      if (base < 0)
        return (c_value_t){-1, C_BAD, NULL};
      c_value_t aggregate = {base, local->type, local->agg};
      return lower_address_aggregate_members(p, aggregate);
    }
    if (!local) {
      c_global_t *global = find_global(p, name);
      if (!global || global->function_symbol) {
        c_function_t *function = find_function(p, name);
        if (function) {
          if (function->has_asm_alias && !function->asm_alias.len) {
            fail(p, "function asm label must be a string literal");
            return (c_value_t){-1, C_BAD, NULL};
          }
          static const char extern_prefix[] = "__nyc_extern_";
          ny_ctok_t link_name =
              function->asm_alias.len ? function->asm_alias : function->name;
          size_t prefix_len = function->asm_alias.len || function->defined
                                  ? 0
                                  : sizeof(extern_prefix) - 1;
          if (link_name.len > SIZE_MAX - prefix_len - 1) {
            fail(p, "function address symbol is too long");
            return (c_value_t){-1, C_BAD, NULL};
          }
          char *symbol = malloc(prefix_len + link_name.len + 1);
          if (!symbol) {
            fail(p, "out of memory lowering a function address");
            return (c_value_t){-1, C_BAD, NULL};
          }
          if (prefix_len)
            memcpy(symbol, extern_prefix, prefix_len);
          memcpy(symbol + prefix_len, link_name.start, link_name.len);
          symbol[prefix_len + link_name.len] = '\0';
          int address = emit_value(p, (nyr_inst_t){.op = NYIR_ADDR_SYMBOL,
                                                   .dst = -1,
                                                   .symbol = symbol});
          free(symbol);
          return address < 0 ? (c_value_t){-1, C_BAD, NULL}
                             : (c_value_t){address, C_FUNC_PTR, NULL};
        }
        fail(p, "address-of requires a declared object");
        return (c_value_t){-1, C_BAD, NULL};
      }
      if (global->has_asm_register) {
        fail(p, "cannot take the address of a global register variable");
        return (c_value_t){-1, C_BAD, NULL};
      }
      static const char data_prefix[] = "__nyc_data_";
      static const char extern_prefix[] = "__nyc_extern_";
      const char *prefix =
          global->is_external || global->is_weak ? extern_prefix : data_prefix;
      size_t prefix_len = strlen(prefix);
      size_t symbol_len = prefix_len + global->name.len;
      char *symbol = malloc(symbol_len + 1);
      if (!symbol) {
        fail(p, "out of memory lowering a file-scope address");
        return (c_value_t){-1, C_BAD, NULL};
      }
      memcpy(symbol, prefix, prefix_len);
      memcpy(symbol + prefix_len, global->name.start, global->name.len);
      symbol[symbol_len] = '\0';
      int address = emit_value(
          p, (nyr_inst_t){.op = NYIR_ADDR_SYMBOL, .dst = -1, .symbol = symbol});
      free(symbol);
      if (address >= 0 && !global->is_array &&
          c_type_is_pointer(global->type) && tok_is(p, p->pos, "[")) {
        int pointer =
            emit_value(p, (nyr_inst_t){.op = NYIR_LOAD_I64,
                                       .dst = -1,
                                       .a = address,
                                       .effects = NYIR_EFFECT_READ_MEMORY |
                                                  NYIR_EFFECT_MAY_TRAP,
                                       .scalar_class = NYIR_SCALAR_I64_RAW});
        c_type_t pointer_type = global->type;
        for (;;) {
          if (!accept(p, "[")) {
            fail(p, "address-of pointer subscript requires an index");
            return (c_value_t){-1, C_BAD, NULL};
          }
          c_value_t index = parse_expr(p, 1);
          if (index.value < 0 || !c_type_is_integer(index.type) ||
              !expect(p, "]")) {
            fail(p, "address-of pointer subscript requires an integer index");
            return (c_value_t){-1, C_BAD, NULL};
          }
          int element = lower_pointer_index_address_for(
              p, pointer_type, global->agg, pointer, index.value);
          if (element < 0)
            return (c_value_t){-1, C_BAD, NULL};
          if (!tok_is(p, p->pos, "[")) {
            if (pointer_type == C_AGG_PTR && tok_is(p, p->pos, ".")) {
              c_value_t aggregate = {element, C_AGG, global->agg};
              return lower_address_aggregate_members(p, aggregate);
            }
            return (c_value_t){element, pointer_type, global->agg};
          }
          c_type_t element_type = pointer_type == C_AGG_PTR_PTR    ? C_AGG_PTR
                                  : pointer_type == C_CHAR_PTR_PTR ? C_CHAR_PTR
                                  : pointer_type == C_FUNC_PTR_PTR
                                      ? C_FUNC_PTR
                                      : C_OPAQUE_PTR;
          pointer =
              emit_value(p, (nyr_inst_t){.op = NYIR_LOAD_I64,
                                         .dst = -1,
                                         .a = element,
                                         .effects = NYIR_EFFECT_READ_MEMORY |
                                                    NYIR_EFFECT_MAY_TRAP,
                                         .scalar_class = NYIR_SCALAR_I64_RAW});
          pointer_type = element_type;
          if (pointer < 0)
            return (c_value_t){-1, C_BAD, NULL};
        }
      }
      if (global->is_array && global->type != C_AGG &&
          global->array_shape.rank > 1 && tok_is(p, p->pos, "[")) {
        c_array_shape_t shape = global->array_shape;
        while (address >= 0 && shape.rank && tok_is(p, p->pos, "["))
          address = lower_array_shape_address(p, address, &shape);
        if (address < 0)
          return (c_value_t){-1, C_BAD, NULL};
        for (size_t i = 0; i < parenthesized_name_closes; ++i)
          if (!expect(p, ")"))
            return (c_value_t){-1, C_BAD, NULL};
        if (shape.rank) {
          shape.pointer_to_array = true;
          if (!remember_array_shape(p, address, shape))
            return (c_value_t){-1, C_BAD, NULL};
        }
        return (c_value_t){address, c_array_pointer_type(shape.element_type),
                           shape.agg};
      }
      if (global->is_array && global->is_pointer_array && accept(p, "[")) {
        c_value_t index = parse_expr(p, 1);
        if (index.value < 0 || !c_type_is_integer(index.type) ||
            !expect(p, "]")) {
          fail(p, "address-of pointer-array element requires an integer index");
          return (c_value_t){-1, C_BAD, NULL};
        }
        c_type_t pointer_type = c_add_pointer_levels(global->type, 1);
        int element = pointer_type == C_BAD
                          ? -1
                          : lower_pointer_index_address(p, global->type,
                                                        address, index.value);
        for (size_t i = 0; i < parenthesized_name_closes; ++i)
          if (!expect(p, ")"))
            return (c_value_t){-1, C_BAD, NULL};
        return element < 0 ? (c_value_t){-1, C_BAD, NULL}
                           : (c_value_t){element, pointer_type, global->agg};
      }
      if (global->is_array && global->type == C_AGG &&
          global->array_shape.rank > 1 && tok_is(p, p->pos, "[")) {
        if (!remember_array_shape(p, address, global->array_shape))
          return (c_value_t){-1, C_BAD, NULL};
        c_value_t element = {address, C_AGG_ARRAY_PTR, global->agg};
        while (array_shape_for_value(p, element.value) &&
               tok_is(p, p->pos, "[")) {
          element = lower_array_shape_index(p, element);
          if (element.value < 0)
            return (c_value_t){-1, C_BAD, NULL};
        }
        if (element.type == C_AGG &&
            (tok_is(p, p->pos, ".") || tok_is(p, p->pos, "->")))
          return lower_address_aggregate_members(p, element);
        if (element.type == C_AGG)
          element.type = C_AGG_PTR;
        for (size_t i = 0; i < parenthesized_name_closes; ++i)
          if (!expect(p, ")"))
            return (c_value_t){-1, C_BAD, NULL};
        return element;
      }
      if (global->is_array && global->type == C_AGG && accept(p, "[")) {
        c_value_t index = parse_expr(p, 1);
        if (index.value < 0 || !c_type_is_integer(index.type) ||
            !expect(p, "]")) {
          fail(p, "address-of aggregate array element needs an integer index");
          return (c_value_t){-1, C_BAD, NULL};
        }
        int stride =
            emit_value(p, (nyr_inst_t){.op = NYIR_CONST_I64,
                                       .dst = -1,
                                       .imm = (int64_t)global->agg->size});
        int offset = stride < 0 ? -1
                                : emit_value(p, (nyr_inst_t){.op = NYIR_MUL_I64,
                                                             .dst = -1,
                                                             .a = index.value,
                                                             .b = stride});
        int element = address < 0 || offset < 0
                          ? -1
                          : emit_value(p, (nyr_inst_t){.op = NYIR_ADD_I64,
                                                       .dst = -1,
                                                       .a = address,
                                                       .b = offset});
        if (element < 0)
          return (c_value_t){-1, C_BAD, NULL};
        if (tok_is(p, p->pos, ".") || tok_is(p, p->pos, "->"))
          return lower_address_aggregate_members(
              p, (c_value_t){element, C_AGG, global->agg});
        for (size_t i = 0; i < parenthesized_name_closes; ++i)
          if (!expect(p, ")"))
            return (c_value_t){-1, C_BAD, NULL};
        return (c_value_t){element, C_AGG_PTR, global->agg};
      }
      if (global->is_array && !global->is_pointer_array &&
          global->type == C_CHAR_PTR && accept(p, "[")) {
        c_value_t index = parse_expr(p, 1);
        if (index.value < 0 || !c_type_is_integer(index.type) ||
            !expect(p, "]")) {
          fail(p, "address-of character-array element needs an integer index");
          return (c_value_t){-1, C_BAD, NULL};
        }
        int element = emit_value(p, (nyr_inst_t){.op = NYIR_ADD_I64,
                                                 .dst = -1,
                                                 .a = address,
                                                 .b = index.value});
        for (size_t i = 0; i < parenthesized_name_closes; ++i)
          if (!expect(p, ")"))
            return (c_value_t){-1, C_BAD, NULL};
        return element < 0 ? (c_value_t){-1, C_BAD, NULL}
                           : (c_value_t){element, C_CHAR_PTR, NULL};
      }
      if (global->is_array && !global->is_pointer_array &&
          global->type != C_AGG && global->type != C_CHAR_PTR &&
          accept(p, "[")) {
        c_value_t index = parse_expr(p, 1);
        if (index.value < 0 || !c_type_is_integer(index.type) ||
            !expect(p, "]")) {
          fail(p, "address-of global array element needs an integer index");
          return (c_value_t){-1, C_BAD, NULL};
        }
        c_type_t pointer_type = c_type_is_pointer(global->type)
                                    ? global->type
                                    : c_add_pointer_levels(global->type, 1);
        c_type_t stride_type = c_type_is_pointer(global->type)
                                   ? global->type
                                   : c_array_pointer_type(global->type);
        int element = pointer_type == C_BAD || stride_type == C_BAD
                          ? -1
                          : lower_pointer_index_address(p, stride_type, address,
                                                        index.value);
        for (size_t i = 0; i < parenthesized_name_closes; ++i)
          if (!expect(p, ")"))
            return (c_value_t){-1, C_BAD, NULL};
        return element < 0 ? (c_value_t){-1, C_BAD, NULL}
                           : (c_value_t){element, pointer_type, NULL};
      }
      if (global->type == C_AGG &&
          (tok_is(p, p->pos, ".") || tok_is(p, p->pos, "->")))
        return lower_address_aggregate_members(
            p, (c_value_t){address, C_AGG_PTR, global->agg});
      if (global->object_seg) {
        c_type_t pointer_type = c_add_pointer_levels(global->type, 1);
        if (address < 0 || pointer_type == C_BAD)
          return (c_value_t){-1, C_BAD, NULL};
        p->last_value_pointee_seg = global->object_seg;
        return (c_value_t){address, pointer_type,
                           c_pointee_marker(global->is_const,
                                            global->is_volatile,
                                            global->object_seg)};
      }
      c_type_t pointer_type =
          global->is_array && !global->is_pointer_array && global->type == C_AGG
              ? C_AGG_ARRAY_PTR
          : global->type == C_AGG      ? C_AGG_PTR
          : global->type == C_CHAR_PTR ? C_CHAR_PTR_PTR
          : global->type == C_I32      ? C_I32_PTR
          : global->type == C_U32      ? C_U32_PTR
          : global->type == C_U64      ? C_U64_PTR
          : global->type == C_F32      ? C_F32_PTR
          : global->type == C_F64      ? C_F64_PTR
                                       : C_OPAQUE_PTR;
      if (address >= 0 && global->is_array && global->array_shape.rank &&
          !remember_array_shape(p, address, global->array_shape))
        return (c_value_t){-1, C_BAD, NULL};
      return address < 0 ? (c_value_t){-1, C_BAD, NULL}
                         : (c_value_t){address, pointer_type, global->agg};
    }
    if (local->pointee_array.pointer_to_array && tok_is(p, p->pos, "[")) {
      c_array_shape_t shape = local->pointee_array;
      int address = lower_local_load(p, local);
      while (address >= 0 && shape.rank && tok_is(p, p->pos, "["))
        address = lower_array_shape_address(p, address, &shape);
      if (address < 0)
        return (c_value_t){-1, C_BAD, NULL};
      for (size_t i = 0; i < parenthesized_name_closes; ++i)
        if (!expect(p, ")"))
          return (c_value_t){-1, C_BAD, NULL};
      if (shape.element_type == C_AGG &&
          ((shape.rank == 1 && tok_is(p, p->pos, "->")) ||
           (!shape.rank && tok_is(p, p->pos, "."))))
        return lower_address_aggregate_members(
            p, (c_value_t){address, shape.rank ? C_AGG_PTR : C_AGG, shape.agg});
      c_type_t pointer = c_array_pointer_type(shape.element_type);
      if (shape.rank) {
        shape.pointer_to_array = true;
        if (!remember_array_shape(p, address, shape))
          return (c_value_t){-1, C_BAD, NULL};
      }
      return pointer == C_BAD ? (c_value_t){-1, C_BAD, NULL}
                              : (c_value_t){address, pointer, shape.agg};
    }
    if (accept(p, "[")) {
      c_value_t index = parse_expr(p, 1);
      if (index.value < 0 || !c_type_is_integer(index.type) ||
          !expect(p, "]")) {
        fail(p, "address-of indexed element requires an integer index");
        return (c_value_t){-1, C_BAD, NULL};
      }
      if (local->array_rank && local->type == C_AGG_PTR && local->agg) {
        int base = lower_local_load(p, local);
        int slot = base < 0 ? -1
                            : lower_pointer_index_address(p, C_AGG_PTR_PTR,
                                                          base, index.value);
        if (slot < 0)
          return (c_value_t){-1, C_BAD, NULL};
        if (tok_is(p, p->pos, "->") || tok_is(p, p->pos, ".")) {
          int object = lower_load_pointer_field(p, slot, false);
          if (object < 0)
            return (c_value_t){-1, C_BAD, NULL};
          return lower_address_aggregate_members(
              p, (c_value_t){object, C_AGG_PTR, local->agg});
        }
        for (size_t pnc = 0; pnc < parenthesized_name_closes; ++pnc)
          if (!expect(p, ")"))
            return (c_value_t){-1, C_BAD, NULL};
        return (c_value_t){slot, C_AGG_PTR_PTR, local->agg};
      }
      if (local->type == C_AGG && local->array_count && local->agg) {
        int base = lower_local_load(p, local);
        int stride =
            emit_value(p, (nyr_inst_t){.op = NYIR_CONST_I64,
                                       .dst = -1,
                                       .imm = (int64_t)local->agg->size});
        int offset = base < 0 || stride < 0
                         ? -1
                         : emit_value(p, (nyr_inst_t){.op = NYIR_MUL_I64,
                                                      .dst = -1,
                                                      .a = index.value,
                                                      .b = stride});
        int address = offset < 0
                          ? -1
                          : emit_value(p, (nyr_inst_t){.op = NYIR_ADD_I64,
                                                       .dst = -1,
                                                       .a = base,
                                                       .b = offset});
        if (address < 0)
          return (c_value_t){-1, C_BAD, NULL};
        if (tok_is(p, p->pos, ".") || tok_is(p, p->pos, "->"))
          return lower_address_aggregate_members(
              p, (c_value_t){address, C_AGG, local->agg});
        for (size_t pnc = 0; pnc < parenthesized_name_closes; ++pnc)
          if (!expect(p, ")"))
            return (c_value_t){-1, C_BAD, NULL};
        return (c_value_t){address, C_AGG_PTR, local->agg};
      }
      if (local->type == C_AGG_PTR && local->agg) {
        int base = lower_local_load(p, local);
        int stride =
            emit_value(p, (nyr_inst_t){.op = NYIR_CONST_I64,
                                       .dst = -1,
                                       .imm = (int64_t)local->agg->size});
        int offset = base < 0 || stride < 0
                         ? -1
                         : emit_value(p, (nyr_inst_t){.op = NYIR_MUL_I64,
                                                      .dst = -1,
                                                      .a = index.value,
                                                      .b = stride});
        int address = offset < 0
                          ? -1
                          : emit_value(p, (nyr_inst_t){.op = NYIR_ADD_I64,
                                                       .dst = -1,
                                                       .a = base,
                                                       .b = offset});
        if (address < 0)
          return (c_value_t){-1, C_BAD, NULL};
        if (tok_is(p, p->pos, ".") || tok_is(p, p->pos, "->"))
          return lower_address_aggregate_members(
              p, (c_value_t){address, C_AGG, local->agg});
        for (size_t pnc = 0; pnc < parenthesized_name_closes; ++pnc)
          if (!expect(p, ")"))
            return (c_value_t){-1, C_BAD, NULL};
        return (c_value_t){address, C_AGG_PTR, local->agg};
      }
      if (local->array_rank > 1) {
        unsigned subscript_count = 1;
        size_t after_subscript = p->pos;
        while (tok_is(p, after_subscript, "[")) {
          size_t after = skip_balanced(p, after_subscript, "[", "]");
          if (after >= p->token_count)
            break;
          ++subscript_count;
          after_subscript = after;
        }
        if (subscript_count == local->array_rank) {
          size_t element_size = c_type_size(p->abi, local->type);
          int base = lower_local_load(p, local);
          int address = base;
          for (unsigned dimension = 0; dimension < local->array_rank;
               ++dimension) {
            c_value_t dimension_index = index;
            if (dimension) {
              if (!expect(p, "["))
                return (c_value_t){-1, C_BAD, NULL};
              dimension_index = parse_expr(p, 1);
              if (dimension_index.value < 0 ||
                  !c_type_is_integer(dimension_index.type) || !expect(p, "]")) {
                fail(p, "address-of multidimensional array element requires "
                        "integer indices");
                return (c_value_t){-1, C_BAD, NULL};
              }
            }
            size_t stride = element_size;
            for (unsigned remaining = dimension + 1;
                 remaining < local->array_rank; ++remaining) {
              size_t extent = local->array_extent[remaining];
              if (!extent || stride > SIZE_MAX / extent) {
                fail(p, "multidimensional array row stride overflows");
                return (c_value_t){-1, C_BAD, NULL};
              }
              stride *= extent;
            }
            if (!stride || stride > INT64_MAX) {
              fail(p, "multidimensional array row stride is unsupported");
              return (c_value_t){-1, C_BAD, NULL};
            }
            address =
                address < 0
                    ? -1
                    : lower_pointer_offset_sized(p, (int64_t)stride, address,
                                                 dimension_index.value, false);
            if (address < 0)
              return (c_value_t){-1, C_BAD, NULL};
          }
          for (size_t pnc = 0; pnc < parenthesized_name_closes; ++pnc)
            if (!expect(p, ")"))
              return (c_value_t){-1, C_BAD, NULL};
          c_type_t pointer_type = c_array_pointer_type(local->type);
          return pointer_type == C_BAD
                     ? (c_value_t){-1, C_BAD, NULL}
                     : (c_value_t){address, pointer_type, NULL};
        }
      }
      if (local->array_count) {
        int base = lower_local_load(p, local);
        c_type_t pointer_type = c_array_pointer_type(local->type);
        int address = base < 0 ? -1
                               : lower_pointer_index_address(p, pointer_type,
                                                             base, index.value);
        if (address >= 0)
          for (size_t pnc = 0; pnc < parenthesized_name_closes; ++pnc)
            if (!expect(p, ")"))
              return (c_value_t){-1, C_BAD, NULL};
        return address < 0 ? (c_value_t){-1, C_BAD, NULL}
                           : (c_value_t){address, pointer_type, NULL};
      }
      bool pointer_array =
          local->type == C_CHAR_PTR_PTR || local->type == C_I8_PTR_PTR ||
          local->type == C_AGG_PTR_PTR || local->type == C_FUNC_PTR_PTR ||
          local->type == C_OPAQUE_PTR_PTR || local->type == C_OPAQUE_PTR ||
          local->type == C_AGG_ARRAY_PTR;
      if (!pointer_array && local->type != C_CHAR_PTR &&
          local->type != C_I8_PTR && local->type != C_I16_PTR &&
          local->type != C_U16_PTR && local->type != C_I32_PTR &&
          local->type != C_U32_PTR && local->type != C_I64_PTR &&
          local->type != C_U64_PTR && local->type != C_F32_PTR &&
          local->type != C_F64_PTR) {
        fail(p, "address-of indexed element requires a supported pointer");
        return (c_value_t){-1, C_BAD, NULL};
      }
      int base = lower_local_load(p, local);
      int address =
          base < 0 ? -1
          : local->type == C_AGG_ARRAY_PTR && local->pointee_array_size
              ? lower_pointer_offset_sized(p,
                                           (int64_t)local->pointee_array_size,
                                           base, index.value, false)
              : lower_pointer_index_address(p, local->type, base, index.value);
      p->last_value_const = false;
      if (address < 0)
        return (c_value_t){-1, C_BAD, NULL};
      if ((local->type == C_AGG_PTR_PTR ||
           (local->type == C_OPAQUE_PTR_PTR &&
            local->pointer_base_type == C_AGG && local->pointer_depth > 1)) &&
          local->agg && tok_is(p, p->pos, "->")) {
        int object =
            emit_value(p, (nyr_inst_t){.op = NYIR_LOAD_I64,
                                       .dst = -1,
                                       .a = address,
                                       .effects = NYIR_EFFECT_READ_MEMORY |
                                                  NYIR_EFFECT_MAY_TRAP,
                                       .scalar_class = NYIR_SCALAR_I64_RAW});
        if (object < 0)
          return (c_value_t){-1, C_BAD, NULL};
        return lower_address_aggregate_members(
            p, (c_value_t){object, C_AGG_PTR, local->agg});
      }
      c_type_t indexed_type = local->type;
      while (accept(p, "[")) {
        c_value_t nested_index = parse_expr(p, 1);
        if (nested_index.value < 0 || !c_type_is_integer(nested_index.type) ||
            !expect(p, "]")) {
          fail(p, "address-of nested pointer index requires an integer");
          return (c_value_t){-1, C_BAD, NULL};
        }
        indexed_type = c_remove_pointer_levels(indexed_type, 1);
        int pointer =
            emit_value(p, (nyr_inst_t){.op = NYIR_LOAD_I64,
                                       .dst = -1,
                                       .a = address,
                                       .effects = NYIR_EFFECT_READ_MEMORY |
                                                  NYIR_EFFECT_MAY_TRAP,
                                       .scalar_class = NYIR_SCALAR_I64_RAW});
        address =
            pointer < 0 || indexed_type == C_BAD
                ? -1
                : lower_pointer_index_address_for(p, indexed_type, local->agg,
                                                  pointer, nested_index.value);
        if (address < 0)
          return (c_value_t){-1, C_BAD, NULL};
      }
      for (size_t pnc = 0; pnc < parenthesized_name_closes; ++pnc)
        if (!expect(p, ")"))
          return (c_value_t){-1, C_BAD, NULL};
      if (indexed_type == C_AGG_PTR && tok_is(p, p->pos, "."))
        return lower_address_aggregate_members(
            p, (c_value_t){address, C_AGG, local->agg});
      return (c_value_t){address, indexed_type, local->agg};
    }
    if (local->array_rank) {
      int address = lower_local_load(p, local);
      return address < 0 ? (c_value_t){-1, C_BAD, NULL}
                         : (c_value_t){address, C_OPAQUE_PTR, NULL};
    }
    if (local->type == C_AGG) {
      int address =
          local->static_name.len
              ? lower_local_load(p, local)
              : emit_value(p,
                           (nyr_inst_t){.op = NYIR_LOAD_LOCAL,
                                        .dst = -1,
                                        .imm = (int64_t)local->slot,
                                        .scalar_class = NYIR_SCALAR_I64_RAW});
      return (c_value_t){address, C_AGG_PTR, local->agg};
    }
    if (local->static_name.len && !local->array_count && local->type != C_AGG) {
      c_type_t pointer_type = c_add_pointer_levels(local->type, 1);
      if (pointer_type == C_BAD)
        return (c_value_t){-1, C_BAD, NULL};
      int address = lower_static_local_address(p, local);
      return address < 0 ? (c_value_t){-1, C_BAD, NULL}
                         : (c_value_t){address, pointer_type, local->agg};
    }
    if (local->type == C_AGG_PTR || local->type == C_CHAR_PTR ||
        local->type == C_I8_PTR || local->type == C_CHAR_PTR_PTR ||
        local->type == C_I8_PTR_PTR || local->type == C_FUNC_PTR_PTR ||
        local->type == C_OPAQUE_PTR_PTR || local->type == C_AGG_PTR_PTR ||
        local->type == C_OPAQUE_PTR || local->type == C_VOID_PTR ||
        local->type == C_FUNC_PTR || local->type == C_I16_PTR ||
        local->type == C_U16_PTR || local->type == C_I32_PTR ||
        local->type == C_U32_PTR || local->type == C_I64_PTR ||
        local->type == C_U64_PTR || local->type == C_F32_PTR ||
        local->type == C_F64_PTR || local->type == C_AGG_ARRAY_PTR) {
      int address =
          emit_value(p, (nyr_inst_t){.op = NYIR_ADDR_LOCAL,
                                     .dst = -1,
                                     .imm = (int64_t)local->slot,
                                     .scalar_class = NYIR_SCALAR_I64_RAW});
      c_type_t pointer_type = C_OPAQUE_PTR;
      if (local->type == C_AGG_PTR)
        pointer_type = C_AGG_PTR_PTR;
      else if (local->type == C_CHAR_PTR)
        pointer_type = C_CHAR_PTR_PTR;
      else if (local->type == C_CHAR_PTR_PTR || local->type == C_I8_PTR_PTR ||
               local->type == C_FUNC_PTR_PTR ||
               local->type == C_OPAQUE_PTR_PTR || local->type == C_AGG_PTR_PTR)
        pointer_type = C_OPAQUE_PTR_PTR;
      else if (local->type == C_OPAQUE_PTR || local->type == C_VOID_PTR)
        pointer_type = C_OPAQUE_PTR_PTR;
      else if (local->type == C_AGG_ARRAY_PTR)
        pointer_type = C_OPAQUE_PTR_PTR;
      else if (local->type == C_I8_PTR || local->type == C_I16_PTR ||
               local->type == C_U16_PTR || local->type == C_I32_PTR ||
               local->type == C_U32_PTR || local->type == C_I64_PTR ||
               local->type == C_U64_PTR || local->type == C_F32_PTR ||
               local->type == C_F64_PTR)
        pointer_type = C_OPAQUE_PTR_PTR;
      return address < 0 ? (c_value_t){-1, C_BAD, NULL}
                         : (c_value_t){address, pointer_type, local->agg};
    }
    if (local->volatile_address >= 0)
      return (c_value_t){local->volatile_address,
                         local->type == C_I32       ? C_I32_PTR
                         : local->type == C_U32     ? C_U32_PTR
                         : local->type == C_U8      ? C_CHAR_PTR
                         : local->type == C_CHAR    ? C_I8_PTR
                         : local->type == C_I16     ? C_I16_PTR
                         : local->type == C_U16     ? C_U16_PTR
                         : local->type == C_I64     ? C_I64_PTR
                         : local->type == C_U64     ? C_U64_PTR
                         : local->type == C_F32     ? C_F32_PTR
                         : local->type == C_F64     ? C_F64_PTR
                         : local->type == C_VA_LIST ? C_AGG_PTR
                                                    : C_BAD,
                         NULL};
    int address =
        emit_value(p, (nyr_inst_t){.op = NYIR_ADDR_LOCAL,
                                   .dst = -1,
                                   .imm = (int64_t)local->slot,
                                   .scalar_class = NYIR_SCALAR_I64_RAW});
    c_type_t pointer_type = local->type == C_I32    ? C_I32_PTR
                            : local->type == C_U32  ? C_U32_PTR
                            : local->type == C_U8   ? C_CHAR_PTR
                            : local->type == C_CHAR ? C_I8_PTR
                            : local->type == C_I16  ? C_I16_PTR
                            : local->type == C_U16  ? C_U16_PTR
                            : local->type == C_I64  ? C_I64_PTR
                            : local->type == C_U64  ? C_U64_PTR
                            : local->type == C_F32  ? C_F32_PTR
                            : local->type == C_F64  ? C_F64_PTR
                                                    : C_BAD;
    if (pointer_type == C_BAD && local->type == C_C64)
      pointer_type = C_C64_PTR;
    else if (pointer_type == C_BAD && local->type == C_C128)
      pointer_type = C_C128_PTR;
    if (address < 0 || pointer_type == C_BAD) {
      if (!p->err[0])
        fail(p, "address-of this local type is unsupported");
      return (c_value_t){-1, C_BAD, NULL};
    }
    return (c_value_t){address, pointer_type, NULL};
  }
  if (accept(p, "*")) {
    if (p->pos + 3 < p->token_count && tok_is(p, p->pos, "(") &&
        tok_is(p, p->pos + 1, "&") && tok_ident(p->tokens[p->pos + 2]) &&
        tok_is(p, p->pos + 3, ")")) {
      ++p->pos;
      ++p->pos;
      c_value_t object = parse_primary(p);
      if (object.value < 0 || !expect(p, ")"))
        return (c_value_t){-1, C_BAD, NULL};
      return object;
    }
    /* Distinguish no cast from a cast explicitly clearing the address space. */
    p->last_value_pointee_seg = UCHAR_MAX;
    c_value_t address = parse_unary(p);
    unsigned char address_segment = p->last_value_pointee_seg;
    p->last_value_pointee_seg = C_SEG_NONE;
    const c_lvalue_t *pointer_lvalue =
        address.value >= 0 ? find_lvalue(p, address.value) : NULL;
    c_local_t *pointer_local =
        pointer_lvalue && pointer_lvalue->is_local
            ? find_local_slot(p, (size_t)pointer_lvalue->local_slot)
            : NULL;
    p->last_value_const = pointer_local ? pointer_local->pointee_const : false;
    const c_array_shape_t *array_shape =
        array_shape_for_value(p, address.value);
    /*
     * A plain aggregate pointer may carry a stale element shape from the
     * object it was derived from; `*p` still names the pointee struct.
     */
    if (array_shape &&
        !(address.type == C_AGG_PTR && !array_shape->pointer_to_array)) {
      if (array_shape->pointer_to_array) {
        c_array_shape_t dereferenced_shape = *array_shape;
        dereferenced_shape.pointer_to_array = false;
        if (!remember_array_shape(p, address.value, dereferenced_shape))
          return (c_value_t){-1, C_BAD, NULL};
      }
      p->last_value_array = true;
      return address;
    }
    if (address.type == C_FUNC_PTR)
      return address;
    if (address.type == C_VA_LIST_PTR)
      return (c_value_t){address.value, C_OPAQUE_PTR, NULL};
    if (address.type == C_AGG_PTR && pointer_local &&
        pointer_local->pointer_base_type == C_VA_LIST)
      return (c_value_t){address.value, C_OPAQUE_PTR, NULL};
    if (address.type == C_CHAR_PTR_PTR) {
      int value =
          emit_value(p, (nyr_inst_t){.op = NYIR_LOAD_I64,
                                     .dst = -1,
                                     .a = address.value,
                                     .effects = NYIR_EFFECT_READ_MEMORY |
                                                NYIR_EFFECT_MAY_TRAP,
                                     .scalar_class = NYIR_SCALAR_I64_RAW});
      if (value < 0 || !record_lvalue(p, value, address.value, C_CHAR_PTR,
                                      c_value_pointee_volatile(address)))
        return (c_value_t){-1, C_BAD, NULL};
      return (c_value_t){value, C_CHAR_PTR, NULL};
    }
    if (address.type == C_I8_PTR_PTR) {
      int value =
          emit_value(p, (nyr_inst_t){.op = NYIR_LOAD_I64,
                                     .dst = -1,
                                     .a = address.value,
                                     .effects = NYIR_EFFECT_READ_MEMORY |
                                                NYIR_EFFECT_MAY_TRAP,
                                     .scalar_class = NYIR_SCALAR_I64_RAW});
      if (value < 0 || !record_lvalue(p, value, address.value, C_I8_PTR,
                                      c_value_pointee_volatile(address)))
        return (c_value_t){-1, C_BAD, NULL};
      return (c_value_t){value, C_I8_PTR, NULL};
    }
    if (address.type == C_AGG_PTR_PTR) {
      /* Aggregate pointers keep their layout in address.agg rather than
       * storing a qualifier marker there. The qualifier applies to this
       * load, not to the ordinary aggregate pointer returned by it. */
      unsigned char segment = address_segment != UCHAR_MAX ? address_segment
                              : pointer_local ? pointer_local->pointee_seg
                                              : C_SEG_NONE;
      int value =
          emit_value(p, (nyr_inst_t){.op = NYIR_LOAD_I64,
                                     .dst = -1,
                                     .a = address.value,
                                     .effects = NYIR_EFFECT_READ_MEMORY |
                                                NYIR_EFFECT_MAY_TRAP,
                                     .flags = c_seg_flags(segment),
                                     .scalar_class = NYIR_SCALAR_I64_RAW});
      if (value < 0 || !record_lvalue(p, value, address.value, C_AGG_PTR,
                                      c_value_pointee_volatile(address)))
        return (c_value_t){-1, C_BAD, NULL};
      p->lvalues[p->lvalue_count - 1].agg = address.agg;
      p->lvalues[p->lvalue_count - 1].segment_flags = c_seg_flags(segment);
      return (c_value_t){value, C_AGG_PTR, address.agg};
    }
    if (address.type == C_AGG_ARRAY_PTR)
      return (c_value_t){address.value, C_AGG_PTR, address.agg};
    if (address.type == C_AGG_PTR_ARRAY_PTR)
      return (c_value_t){address.value, C_AGG_PTR_PTR, address.agg};
    if (pointer_local && pointer_local->pointee_array_size &&
        c_type_is_pointer(address.type)) {
      p->last_value_array = true;
      if (pointer_local->pointee_array.rank &&
          !remember_array_shape(p, address.value, pointer_local->pointee_array))
        return (c_value_t){-1, C_BAD, NULL};
      return address;
    }
    if (address.type == C_FUNC_PTR_PTR || address.type == C_U16_PTR_PTR ||
        address.type == C_I64_PTR_PTR || address.type == C_U64_PTR_PTR ||
        address.type == C_OPAQUE_PTR_PTR ||
        address.type == C_OPAQUE_PTR_PTR_PTR) {
      int value =
          emit_value(p, (nyr_inst_t){.op = NYIR_LOAD_I64,
                                     .dst = -1,
                                     .a = address.value,
                                     .effects = NYIR_EFFECT_READ_MEMORY |
                                                NYIR_EFFECT_MAY_TRAP,
                                     .scalar_class = NYIR_SCALAR_I64_RAW});
      c_type_t pointee_type = address.type == C_FUNC_PTR_PTR  ? C_FUNC_PTR
                              : address.type == C_U16_PTR_PTR ? C_U16_PTR
                              : address.type == C_I64_PTR_PTR ? C_I64_PTR
                              : address.type == C_U64_PTR_PTR ? C_U64_PTR
                              : address.type == C_OPAQUE_PTR_PTR_PTR
                                  ? C_OPAQUE_PTR_PTR
                                  : C_OPAQUE_PTR;
      if ((address.type == C_OPAQUE_PTR_PTR ||
           address.type == C_OPAQUE_PTR_PTR_PTR) &&
          pointer_local && pointer_local->pointer_depth > 1) {
        pointee_type = c_add_pointer_levels(pointer_local->pointer_base_type,
                                            pointer_local->pointer_depth - 1);
        if (pointee_type == C_BAD) {
          fail(p, "dereference has an unsupported nested pointer type");
          return (c_value_t){-1, C_BAD, NULL};
        }
      }
      if (value < 0 || !record_lvalue(p, value, address.value, pointee_type,
                                      c_value_pointee_volatile(address)))
        return (c_value_t){-1, C_BAD, NULL};
      return (c_value_t){value, pointee_type, NULL};
    }
    if (address.type == C_OPAQUE_PTR) {
      int value =
          emit_value(p, (nyr_inst_t){.op = NYIR_LOAD_I64,
                                     .dst = -1,
                                     .a = address.value,
                                     .effects = NYIR_EFFECT_READ_MEMORY |
                                                NYIR_EFFECT_MAY_TRAP,
                                     .scalar_class = NYIR_SCALAR_I64_RAW});
      if (value < 0 || !record_lvalue(p, value, address.value, C_OPAQUE_PTR,
                                      c_value_pointee_volatile(address)))
        return (c_value_t){-1, C_BAD, NULL};
      return (c_value_t){value, C_OPAQUE_PTR, NULL};
    }
    if (address.type == C_AGG_PTR) {
      if (pointer_local && pointer_local->pointee_array_size)
        return (c_value_t){address.value, C_AGG_PTR, address.agg};
      /*
       * Dereferencing a struct pointer yields the struct itself (still
       * addressed in place).
       */
      c_value_t pointee = {address.value, C_AGG, address.agg};
      c_value_t member;
      if (!lower_member_rvalue(p, pointee, &member))
        return (c_value_t){-1, C_BAD, NULL};
      return member;
    }
    if (address.type == C_C64_PTR || address.type == C_C128_PTR ||
        address.type == C_C_LDOUBLE_PTR) {
      static c_agg_type_t complex_ptr_f32 = {
          .scope_id = SIZE_MAX,
          .complete = true,
          .size = 8,
          .align = 4,
          .field_count = 2,
          .fields = {
              {.kind = C_AGG_FIELD_F32, .offset = 0, .size = 4, .align = 4},
              {.kind = C_AGG_FIELD_F32, .offset = 4, .size = 4, .align = 4}}};
      static c_agg_type_t complex_ptr_f64 = {
          .scope_id = SIZE_MAX,
          .complete = true,
          .size = 16,
          .align = 8,
          .field_count = 2,
          .fields = {
              {.kind = C_AGG_FIELD_F64, .offset = 0, .size = 8, .align = 8},
              {.kind = C_AGG_FIELD_F64, .offset = 8, .size = 8, .align = 8}}};
      static c_agg_type_t complex_ptr_ldouble = {
          .scope_id = SIZE_MAX,
          .complete = true,
          .size = 16,
          .align = 8,
          .field_count = 2,
          .fields = {
              {.kind = C_AGG_FIELD_F64, .offset = 0, .size = 8, .align = 8},
              {.kind = C_AGG_FIELD_F64, .offset = 8, .size = 8, .align = 8}}};
      c_agg_type_t *pointee_agg = address.type == C_C64_PTR ? &complex_ptr_f32
                                  : address.type == C_C128_PTR
                                      ? &complex_ptr_f64
                                      : &complex_ptr_ldouble;
      if (!record_lvalue(p, address.value, address.value, C_AGG,
                         c_value_pointee_volatile(address)))
        return (c_value_t){-1, C_BAD, NULL};
      p->lvalues[p->lvalue_count - 1].agg = pointee_agg;
      return (c_value_t){address.value, C_AGG, pointee_agg};
    }
    if (address.type != C_CHAR_PTR && address.type != C_I8_PTR &&
        address.type != C_I16_PTR && address.type != C_U16_PTR &&
        address.type != C_I32_PTR && address.type != C_U32_PTR &&
        address.type != C_I64_PTR && address.type != C_U64_PTR &&
        address.type != C_F32_PTR && address.type != C_F64_PTR) {
      char message[128];
      snprintf(message, sizeof(message),
               "dereference requires a supported scalar pointer (type %d)",
               (int)address.type);
      fail(p, message);
      return (c_value_t){-1, C_BAD, NULL};
    }
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
    if (element_type == C_I16 || element_type == C_U16) {
      int value = lower_load_u16_field_seg(p, address.value, element_type,
                                           c_value_pointee_volatile(address),
                                           c_value_seg_flags(address));
      if (value < 0 || !record_lvalue(p, value, address.value, element_type,
                                      c_value_pointee_volatile(address)))
        return (c_value_t){-1, C_BAD, NULL};
      p->lvalues[p->lvalue_count - 1].segment_flags =
          c_value_seg_flags(address);
      return (c_value_t){value, element_type, NULL};
    }
    unsigned memory_flags =
        (address.type == C_CHAR_PTR || address.type == C_I8_PTR
             ? NYIR_INST_F_MEM_BYTE
             : (address.type == C_I32_PTR || address.type == C_U32_PTR ||
                        address.type == C_F32_PTR
                    ? NYIR_INST_F_MEM_I32
                    : 0)) |
        c_value_seg_flags(address);
    int value = emit_value(
        p,
        (nyr_inst_t){
            .op = NYIR_LOAD_I64,
            .dst = -1,
            .a = address.value,
            .imm = 0,
            .effects =
                NYIR_EFFECT_READ_MEMORY | NYIR_EFFECT_MAY_TRAP |
                (c_value_pointee_volatile(address) ? NYIR_EFFECT_VOLATILE : 0),
            .flags = memory_flags,
            .scalar_class = c_type_is_float(element_type)
                                ? c_float_scalar_class(element_type)
                                : NYIR_SCALAR_I64_RAW});
    if (value >= 0 && (element_type == C_CHAR || element_type == C_I32))
      value = normalize_integer(p, element_type, value);
    if (value >= 0) {
      if (!record_lvalue(p, value, address.value, element_type,
                         c_value_pointee_volatile(address)))
        return (c_value_t){-1, C_BAD, NULL};
      p->lvalues[p->lvalue_count - 1].segment_flags =
          c_value_seg_flags(address);
    }
    return (c_value_t){.value = value, .type = element_type};
  }
  if (accept(p, "+"))
    return parse_unary(p);
  if (accept(p, "-") || accept(p, "!") || accept(p, "~")) {
    ny_ctok_t op = p->tokens[p->pos - 1];
    c_value_t value = parse_unary(p);
    if (value.value < 0)
      return value;
    if (ny_ctok_eq(op, "-") && value.type == C_AGG && value.agg &&
        value.agg->scope_id == SIZE_MAX && value.agg->size != 32) {
      bool neg_single = value.agg->size == 8;
      size_t neg_lane = neg_single ? 4 : 8;
      int real = neg_single ? lower_load_f32_field(p, value.value, false)
                            : lower_load_field(p, value.value, true, false);
      int imag_addr = lower_add_const(p, value.value, neg_lane);
      int imag = imag_addr < 0 ? -1
                 : neg_single  ? lower_load_f32_field(p, imag_addr, false)
                               : lower_load_field(p, imag_addr, true, false);
      int zero = emit_value(
          p, (nyr_inst_t){.op = neg_single ? NYIR_CONST_F32 : NYIR_CONST_F64,
                          .dst = -1,
                          .imm = 0,
                          .scalar_class =
                              neg_single ? NYIR_SCALAR_F32 : NYIR_SCALAR_F64});
      int neg_real =
          real < 0
              ? -1
              : emit_value(
                    p,
                    (nyr_inst_t){.op = neg_single ? NYIR_SUB_F32 : NYIR_SUB_F64,
                                 .dst = -1,
                                 .a = zero,
                                 .b = real,
                                 .scalar_class = neg_single ? NYIR_SCALAR_F32
                                                            : NYIR_SCALAR_F64});
      int neg_imag =
          imag < 0
              ? -1
              : emit_value(
                    p,
                    (nyr_inst_t){.op = neg_single ? NYIR_SUB_F32 : NYIR_SUB_F64,
                                 .dst = -1,
                                 .a = zero,
                                 .b = imag,
                                 .scalar_class = neg_single ? NYIR_SCALAR_F32
                                                            : NYIR_SCALAR_F64});
      size_t offset = 0;
      if (zero < 0 || neg_real < 0 || neg_imag < 0 ||
          !agg_area_alloc(p, value.agg->size, &offset))
        return (c_value_t){-1, C_BAD, NULL};
      int base = p->agg_base;
      int imag_target = lower_add_const(p, base, neg_lane);
      bool stored =
          imag_target >= 0 &&
          (neg_single
               ? lower_store_f32_field(p, base, neg_real, false) &&
                     lower_store_f32_field(p, imag_target, neg_imag, false)
               : lower_store_field(p, base, neg_real, true, false) &&
                     lower_store_field(p, imag_target, neg_imag, true, false));
      if (!stored)
        return (c_value_t){-1, C_BAD, NULL};
      return (c_value_t){base, C_AGG, value.agg};
    }
    if (ny_ctok_eq(op, "!")) {
      bool is_float = c_type_is_float(value.type);
      c_type_t float_type = is_float ? value.type : C_F64;
      int zero = emit_value(
          p, (nyr_inst_t){.op = value.type == C_F32   ? NYIR_CONST_F32
                                : value.type == C_F64 ? NYIR_CONST_F64
                                                      : NYIR_CONST_I64,
                          .dst = -1,
                          .scalar_class = is_float
                                              ? c_float_scalar_class(float_type)
                                              : NYIR_SCALAR_I64_RAW});
      int result =
          emit_value(p, (nyr_inst_t){.op = value.type == C_F32   ? NYIR_CMP_F32
                                           : value.type == C_F64 ? NYIR_CMP_F64
                                                                 : NYIR_CMP_I64,
                                     .dst = -1,
                                     .a = value.value,
                                     .b = zero,
                                     .scalar_class = NYIR_SCALAR_I64_RAW,
                                     .cmp = NYIR_CMP_EQ});
      return (c_value_t){result, C_I64, NULL};
    }
    if (ny_ctok_eq(op, "~")) {
      if (!c_type_is_integer(value.type)) {
        fail(p, "bitwise complement requires an integer");
        return (c_value_t){-1, C_BAD, NULL};
      }
      c_type_t result_type = value.type;
      if (result_type == C_CHAR || result_type == C_U8 ||
          result_type == C_I16 || result_type == C_U16)
        result_type = C_I32;
      int operand = result_type == value.type
                        ? value.value
                        : normalize_integer(p, result_type, value.value);
      if (operand < 0)
        return (c_value_t){-1, C_BAD, NULL};
      int all = emit_value(
          p, (nyr_inst_t){.op = NYIR_CONST_I64, .dst = -1, .imm = -1});
      int inverted = all < 0 ? -1
                             : emit_value(p, (nyr_inst_t){.op = NYIR_XOR_I64,
                                                          .dst = -1,
                                                          .a = operand,
                                                          .b = all});
      if (inverted >= 0)
        inverted = normalize_integer(p, result_type, inverted);
      return (c_value_t){inverted, result_type, NULL};
    }
    if (c_type_is_pointer(value.type) ||
        (!c_type_is_integer(value.type) && !c_type_is_float(value.type))) {
      fail(p, "unary minus requires an arithmetic scalar");
      return (c_value_t){-1, C_BAD, NULL};
    }
    int zero = emit_value(
        p, (nyr_inst_t){.op = value.type == C_F32   ? NYIR_CONST_F32
                              : value.type == C_F64 ? NYIR_CONST_F64
                                                    : NYIR_CONST_I64,
                        .dst = -1,
                        .scalar_class = c_type_is_float(value.type)
                                            ? c_float_scalar_class(value.type)
                                            : NYIR_SCALAR_I64_RAW});
    return (c_value_t){
        emit_value(p, (nyr_inst_t){.op = value.type == C_F32   ? NYIR_SUB_F32
                                         : value.type == C_F64 ? NYIR_SUB_F64
                                                               : NYIR_SUB_I64,
                                   .dst = -1,
                                   .a = zero,
                                   .b = value.value,
                                   .scalar_class =
                                       c_type_is_float(value.type)
                                           ? c_float_scalar_class(value.type)
                                           : NYIR_SCALAR_I64_RAW}),
        value.type, NULL};
  }
  return parse_postfix(p);
}

static c_value_t convert_numeric_float(c_parser_t *p, c_value_t value,
                                       c_type_t target) {
  if (value.type == target)
    return value;
  /*
   * Complex operands are handled by the aggregate arithmetic path; leave them
   * untouched so the binary operator can promote the real side instead.
   */
  if (value.type == C_AGG)
    return value;
  if (c_type_is_integer(value.type)) {
    if (value.type == C_U64) {
      nyr_inst_t call = {.op = NYIR_CALL,
                         .dst = -1,
                         .a = value.value,
                         .imm = 1,
                         .flags = NYIR_INST_F_EXTERN,
                         .effects = NYIR_EFFECT_CALL,
                         .scalar_class = c_float_scalar_class(target),
                         .symbol = target == C_F32 ? "__floatundisf"
                                                   : "__floatundidf"};
      nyr_call_arg_class_set(call.arg_scalar_classes, 0, NYIR_SCALAR_I64_RAW);
      value.value = emit_value(p, call);
    } else {
      value.value = emit_value(
          p, (nyr_inst_t){.op = target == C_F32 ? NYIR_I64_TO_F32
                                                : NYIR_I64_TO_F64,
                          .dst = -1,
                          .a = value.value,
                          .scalar_class = c_float_scalar_class(target)});
    }
  } else if (value.type == C_F32 && target == C_F64) {
    value.value = emit_value(p, (nyr_inst_t){.op = NYIR_F32_TO_F64,
                                             .dst = -1,
                                             .a = value.value,
                                             .scalar_class = NYIR_SCALAR_F64});
  } else if (value.type == C_F64 && target == C_F32) {
    value.value = emit_value(p, (nyr_inst_t){.op = NYIR_F64_TO_F32,
                                             .dst = -1,
                                             .a = value.value,
                                             .scalar_class = NYIR_SCALAR_F32});
  } else {
    fail(p, "unsupported arithmetic conversion to floating point");
    return (c_value_t){-1, C_BAD, NULL};
  }
  return value.value < 0 ? (c_value_t){-1, C_BAD, NULL}
                         : (c_value_t){value.value, target, NULL};
}

#include "code/lang/c/lower/parts/int128.h"

static int lower_int128_unsigned_lt(c_parser_t *p, int left, int right) {
  int sign = emit_i64_constant(p, INT64_MIN);
  int ordered_left = sign < 0 ? -1
                              : emit_value(p, (nyr_inst_t){.op = NYIR_XOR_I64,
                                                           .dst = -1,
                                                           .a = left,
                                                           .b = sign});
  int ordered_right = sign < 0 ? -1
                               : emit_value(p, (nyr_inst_t){.op = NYIR_XOR_I64,
                                                            .dst = -1,
                                                            .a = right,
                                                            .b = sign});
  if (ordered_left < 0 || ordered_right < 0)
    return -1;
  return emit_i64_compare(p, ordered_left, ordered_right, NYIR_CMP_LT);
}

static c_value_t lower_int128_bitwise(c_parser_t *p, c_value_t value,
                                      c_value_t other, nyr_op_t op) {
  if (!value.agg || !value.agg->is_int128)
    return (c_value_t){-1, C_BAD, NULL};
  int low = lower_int128_word(p, value.value, 0);
  int high = lower_int128_word(p, value.value, 8);
  size_t offset = 0;
  int result = lower_int128_allocate(p, value.agg, &offset);
  if (low < 0 || high < 0 || result < 0 ||
      lower_int128_store(p, result, 0, low) < 0 ||
      lower_int128_store(p, result, 8, high) < 0 ||
      !lower_int128_compound_bitwise(p, result, value.agg, other, op))
    return (c_value_t){-1, C_BAD, NULL};
  return (c_value_t){result, C_AGG, value.agg};
}

static c_value_t lower_int128_compare(c_parser_t *p, c_value_t left,
                                      c_value_t right, ny_ctok_t op) {
  const c_agg_type_t *wide_type = left.type == C_AGG    ? left.agg
                                  : right.type == C_AGG ? right.agg
                                                        : NULL;
  if (!wide_type || !wide_type->is_int128)
    return (c_value_t){-1, C_BAD, NULL};
  if (left.type != C_AGG) {
    size_t offset = 0;
    int extended = lower_int128_allocate(p, wide_type, &offset);
    if (extended < 0 || !lower_int128_store_integer(p, extended, left))
      return (c_value_t){-1, C_BAD, NULL};
    left = (c_value_t){extended, C_AGG, wide_type};
  }
  if (right.type != C_AGG) {
    size_t offset = 0;
    int extended = lower_int128_allocate(p, wide_type, &offset);
    if (extended < 0 || !lower_int128_store_integer(p, extended, right))
      return (c_value_t){-1, C_BAD, NULL};
    right = (c_value_t){extended, C_AGG, wide_type};
  }
  if (!left.agg || !left.agg->is_int128 || !right.agg || !right.agg->is_int128)
    return (c_value_t){-1, C_BAD, NULL};
  int left_low = lower_int128_word(p, left.value, 0);
  int left_high = lower_int128_word(p, left.value, 8);
  int right_low = lower_int128_word(p, right.value, 0);
  int right_high = lower_int128_word(p, right.value, 8);
  bool is_unsigned = wide_type->fields[0].kind == C_AGG_FIELD_U64;
  int high_less = is_unsigned
                      ? lower_int128_unsigned_lt(p, left_high, right_high)
                      : emit_i64_compare(p, left_high, right_high, NYIR_CMP_LT);
  int high_equal = emit_i64_compare(p, left_high, right_high, NYIR_CMP_EQ);
  int low_less = lower_int128_unsigned_lt(p, left_low, right_low);
  int low_equal = emit_i64_compare(p, left_low, right_low, NYIR_CMP_EQ);
  if (left_low < 0 || left_high < 0 || right_low < 0 || right_high < 0 ||
      high_less < 0 || high_equal < 0 || low_less < 0 || low_equal < 0)
    return (c_value_t){-1, C_BAD, NULL};
  int equal = emit_value(
      p, (nyr_inst_t){
             .op = NYIR_AND_I64, .dst = -1, .a = high_equal, .b = low_equal});
  int lower_less = emit_value(
      p, (nyr_inst_t){
             .op = NYIR_AND_I64, .dst = -1, .a = high_equal, .b = low_less});
  int less = emit_value(
      p, (nyr_inst_t){
             .op = NYIR_OR_I64, .dst = -1, .a = high_less, .b = lower_less});
  int one = emit_i64_constant(p, 1);
  if (equal < 0 || lower_less < 0 || less < 0 || one < 0)
    return (c_value_t){-1, C_BAD, NULL};
  int result = -1;
  if (ny_ctok_eq(op, "=="))
    result = equal;
  else if (ny_ctok_eq(op, "!="))
    result = emit_value(
        p, (nyr_inst_t){.op = NYIR_XOR_I64, .dst = -1, .a = equal, .b = one});
  else if (ny_ctok_eq(op, "<"))
    result = less;
  else if (ny_ctok_eq(op, ">="))
    result = emit_value(
        p, (nyr_inst_t){.op = NYIR_XOR_I64, .dst = -1, .a = less, .b = one});
  else {
    int less_equal = emit_value(
        p, (nyr_inst_t){.op = NYIR_OR_I64, .dst = -1, .a = less, .b = equal});
    result = ny_ctok_eq(op, "<=") ? less_equal
             : less_equal < 0 ? -1
                              : emit_value(p, (nyr_inst_t){.op = NYIR_XOR_I64,
                                                           .dst = -1,
                                                           .a = less_equal,
                                                           .b = one});
  }
  return result < 0 ? (c_value_t){-1, C_BAD, NULL}
                    : (c_value_t){result, C_I32, NULL};
}

static c_value_t lower_int128_mul_u32(c_parser_t *p, c_value_t value,
                                      c_value_t multiplier) {
  if (multiplier.type != C_U32 && multiplier.type != C_I32)
    return (c_value_t){-1, C_BAD, NULL};
  int low = lower_int128_word(p, value.value, 0);
  int high = lower_int128_word(p, value.value, 8);
  int factor = normalize_u32(p, multiplier.value);
  int mask = emit_i64_constant(p, UINT32_MAX);
  int thirty_two = emit_i64_constant(p, 32);
  if (low < 0 || high < 0 || factor < 0 || mask < 0 || thirty_two < 0)
    return (c_value_t){-1, C_BAD, NULL};
  int lo32 = emit_value(
      p, (nyr_inst_t){.op = NYIR_AND_I64, .dst = -1, .a = low, .b = mask});
  int hi32 = lower_int128_ushr(p, low, thirty_two);
  int p0 = emit_value(
      p, (nyr_inst_t){.op = NYIR_MUL_I64, .dst = -1, .a = lo32, .b = factor});
  int p1 = emit_value(
      p, (nyr_inst_t){.op = NYIR_MUL_I64, .dst = -1, .a = hi32, .b = factor});
  int p0_high = lower_int128_ushr(p, p0, thirty_two);
  int p1_low = emit_value(
      p, (nyr_inst_t){.op = NYIR_AND_I64, .dst = -1, .a = p1, .b = mask});
  int middle = emit_value(
      p,
      (nyr_inst_t){.op = NYIR_ADD_I64, .dst = -1, .a = p0_high, .b = p1_low});
  int low_low = emit_value(
      p, (nyr_inst_t){.op = NYIR_AND_I64, .dst = -1, .a = p0, .b = mask});
  int low_high = emit_value(
      p, (nyr_inst_t){
             .op = NYIR_SHL_I64, .dst = -1, .a = middle, .b = thirty_two});
  int result_low = emit_value(
      p,
      (nyr_inst_t){.op = NYIR_OR_I64, .dst = -1, .a = low_low, .b = low_high});
  int p1_high = lower_int128_ushr(p, p1, thirty_two);
  int carry = lower_int128_ushr(p, middle, thirty_two);
  int result_high = emit_value(
      p, (nyr_inst_t){.op = NYIR_ADD_I64, .dst = -1, .a = p1_high, .b = carry});
  int high_product = emit_value(
      p, (nyr_inst_t){.op = NYIR_MUL_I64, .dst = -1, .a = high, .b = factor});
  result_high = emit_value(p, (nyr_inst_t){.op = NYIR_ADD_I64,
                                           .dst = -1,
                                           .a = result_high,
                                           .b = high_product});
  if (result_low < 0 || result_high < 0)
    return (c_value_t){-1, C_BAD, NULL};
  size_t offset = 0;
  int result = lower_int128_allocate(p, value.agg, &offset);
  if (result < 0 || lower_int128_store(p, result, 0, result_low) < 0 ||
      lower_int128_store(p, result, 8, result_high) < 0)
    return (c_value_t){-1, C_BAD, NULL};
  return (c_value_t){result, C_AGG, value.agg};
}

static c_value_t lower_int128_mul_u64(c_parser_t *p, c_value_t value,
                                      c_value_t multiplier) {
  if (!c_type_is_integer(multiplier.type))
    return (c_value_t){-1, C_BAD, NULL};
  int low = lower_int128_word(p, value.value, 0);
  int high = lower_int128_word(p, value.value, 8);
  int factor = normalize_integer(p, C_U64, multiplier.value);
  int mask = emit_i64_constant(p, UINT32_MAX);
  int thirty_two = emit_i64_constant(p, 32);
  if (low < 0 || high < 0 || factor < 0 || mask < 0 || thirty_two < 0)
    return (c_value_t){-1, C_BAD, NULL};
  int left_low = emit_value(
      p, (nyr_inst_t){.op = NYIR_AND_I64, .dst = -1, .a = low, .b = mask});
  int left_high = lower_int128_ushr(p, low, thirty_two);
  int right_low = emit_value(
      p, (nyr_inst_t){.op = NYIR_AND_I64, .dst = -1, .a = factor, .b = mask});
  int right_high = lower_int128_ushr(p, factor, thirty_two);
  int p00 = emit_value(
      p, (nyr_inst_t){
             .op = NYIR_MUL_I64, .dst = -1, .a = left_low, .b = right_low});
  int p01 = emit_value(
      p, (nyr_inst_t){
             .op = NYIR_MUL_I64, .dst = -1, .a = left_low, .b = right_high});
  int p10 = emit_value(
      p, (nyr_inst_t){
             .op = NYIR_MUL_I64, .dst = -1, .a = left_high, .b = right_low});
  int p11 = emit_value(
      p, (nyr_inst_t){
             .op = NYIR_MUL_I64, .dst = -1, .a = left_high, .b = right_high});
  int p00_high = lower_int128_ushr(p, p00, thirty_two);
  int p01_low = emit_value(
      p, (nyr_inst_t){.op = NYIR_AND_I64, .dst = -1, .a = p01, .b = mask});
  int p10_low = emit_value(
      p, (nyr_inst_t){.op = NYIR_AND_I64, .dst = -1, .a = p10, .b = mask});
  int middle = emit_value(
      p,
      (nyr_inst_t){.op = NYIR_ADD_I64, .dst = -1, .a = p00_high, .b = p01_low});
  middle = emit_value(
      p,
      (nyr_inst_t){.op = NYIR_ADD_I64, .dst = -1, .a = middle, .b = p10_low});
  int low_low = emit_value(
      p, (nyr_inst_t){.op = NYIR_AND_I64, .dst = -1, .a = p00, .b = mask});
  int low_high = emit_value(
      p, (nyr_inst_t){
             .op = NYIR_SHL_I64, .dst = -1, .a = middle, .b = thirty_two});
  int result_low = emit_value(
      p,
      (nyr_inst_t){.op = NYIR_OR_I64, .dst = -1, .a = low_low, .b = low_high});
  int p01_high = lower_int128_ushr(p, p01, thirty_two);
  int p10_high = lower_int128_ushr(p, p10, thirty_two);
  int middle_high = lower_int128_ushr(p, middle, thirty_two);
  int result_high = emit_value(
      p, (nyr_inst_t){.op = NYIR_ADD_I64, .dst = -1, .a = p11, .b = p01_high});
  result_high = emit_value(
      p, (nyr_inst_t){
             .op = NYIR_ADD_I64, .dst = -1, .a = result_high, .b = p10_high});
  result_high = emit_value(p, (nyr_inst_t){.op = NYIR_ADD_I64,
                                           .dst = -1,
                                           .a = result_high,
                                           .b = middle_high});
  int upper_product = emit_value(
      p, (nyr_inst_t){.op = NYIR_MUL_I64, .dst = -1, .a = high, .b = factor});
  result_high = emit_value(p, (nyr_inst_t){.op = NYIR_ADD_I64,
                                           .dst = -1,
                                           .a = result_high,
                                           .b = upper_product});
  if (result_low < 0 || result_high < 0)
    return (c_value_t){-1, C_BAD, NULL};
  size_t offset = 0;
  int result = lower_int128_allocate(p, value.agg, &offset);
  if (result < 0 || lower_int128_store(p, result, 0, result_low) < 0 ||
      lower_int128_store(p, result, 8, result_high) < 0)
    return (c_value_t){-1, C_BAD, NULL};
  return (c_value_t){result, C_AGG, value.agg};
}

static c_value_t lower_int128_add_u64(c_parser_t *p, c_value_t value,
                                      c_value_t addend) {
  if (!c_type_is_integer(addend.type))
    return (c_value_t){-1, C_BAD, NULL};
  int low = lower_int128_word(p, value.value, 0);
  int high = lower_int128_word(p, value.value, 8);
  int rhs = normalize_integer(p, C_U64, addend.value);
  if (low < 0 || high < 0 || rhs < 0)
    return (c_value_t){-1, C_BAD, NULL};
  int result_low = emit_value(
      p, (nyr_inst_t){.op = NYIR_ADD_I64, .dst = -1, .a = low, .b = rhs});
  int carry = lower_int128_unsigned_lt(p, result_low, low);
  int result_high = emit_value(
      p, (nyr_inst_t){.op = NYIR_ADD_I64, .dst = -1, .a = high, .b = carry});
  if (result_low < 0 || carry < 0 || result_high < 0)
    return (c_value_t){-1, C_BAD, NULL};
  size_t offset = 0;
  int result = lower_int128_allocate(p, value.agg, &offset);
  if (result < 0 || lower_int128_store(p, result, 0, result_low) < 0 ||
      lower_int128_store(p, result, 8, result_high) < 0)
    return (c_value_t){-1, C_BAD, NULL};
  return (c_value_t){result, C_AGG, value.agg};
}

static c_value_t lower_int128_shift_left(c_parser_t *p, c_value_t value,
                                         c_value_t count_value) {
  if (!value.agg || !value.agg->is_int128 ||
      !c_type_is_integer(count_value.type) || p->next_label > INT_MAX - 4)
    return (c_value_t){-1, C_BAD, NULL};
  int low = lower_int128_word(p, value.value, 0);
  int high = lower_int128_word(p, value.value, 8);
  int count = normalize_integer(p, C_U64, count_value.value);
  int sixty_four = emit_i64_constant(p, 64);
  int zero = emit_i64_constant(p, 0);
  if (low < 0 || high < 0 || count < 0 || sixty_four < 0 || zero < 0)
    return (c_value_t){-1, C_BAD, NULL};
  int is_small = lower_int128_unsigned_lt(p, count, sixty_four);
  int is_zero = emit_i64_compare(p, count, zero, NYIR_CMP_EQ);
  if (is_small < 0 || is_zero < 0)
    return (c_value_t){-1, C_BAD, NULL};
  size_t small_label = (size_t)p->next_label++;
  size_t zero_label = (size_t)p->next_label++;
  size_t large_label = (size_t)p->next_label++;
  size_t merge_label = (size_t)p->next_label++;
  size_t offset = 0;
  int result = lower_int128_allocate(p, value.agg, &offset);
  if (result < 0 || !emit_branch_if(p, is_small, (int64_t)small_label) ||
      !emit_branch(p, (int64_t)large_label) ||
      !emit_label(p, (int64_t)small_label) ||
      !emit_branch_if(p, is_zero, (int64_t)zero_label))
    return (c_value_t){-1, C_BAD, NULL};
  int complement = emit_i64_binary(p, NYIR_SUB_I64, sixty_four, count);
  int low_result = emit_i64_binary(p, NYIR_SHL_I64, low, count);
  int high_low_bits = lower_int128_ushr(p, low, complement);
  int shifted_high = emit_i64_binary(p, NYIR_SHL_I64, high, count);
  int high_result =
      emit_i64_binary(p, NYIR_OR_I64, shifted_high, high_low_bits);
  if (complement < 0 || low_result < 0 || high_low_bits < 0 ||
      shifted_high < 0 || high_result < 0 ||
      lower_int128_store(p, result, 0, low_result) < 0 ||
      lower_int128_store(p, result, 8, high_result) < 0 ||
      !emit_branch(p, (int64_t)merge_label) ||
      !emit_label(p, (int64_t)zero_label) ||
      lower_int128_store(p, result, 0, low) < 0 ||
      lower_int128_store(p, result, 8, high) < 0 ||
      !emit_branch(p, (int64_t)merge_label) ||
      !emit_label(p, (int64_t)large_label))
    return (c_value_t){-1, C_BAD, NULL};
  int count_minus_sixty_four =
      emit_i64_binary(p, NYIR_SUB_I64, count, sixty_four);
  int large_high =
      emit_i64_binary(p, NYIR_SHL_I64, low, count_minus_sixty_four);
  if (count_minus_sixty_four < 0 || large_high < 0 ||
      lower_int128_store(p, result, 0, zero) < 0 ||
      lower_int128_store(p, result, 8, large_high) < 0 ||
      !emit_label(p, (int64_t)merge_label))
    return (c_value_t){-1, C_BAD, NULL};
  return (c_value_t){result, C_AGG, value.agg};
}

static c_value_t lower_int128_shift_right(c_parser_t *p, c_value_t value,
                                          c_value_t count_value) {
  if (!c_type_is_integer(count_value.type) || p->next_label > INT_MAX - 4)
    return (c_value_t){-1, C_BAD, NULL};
  int low = lower_int128_word(p, value.value, 0);
  int high = lower_int128_word(p, value.value, 8);
  int count = normalize_integer(p, C_U64, count_value.value);
  int sixty_four = emit_i64_constant(p, 64);
  int zero = emit_i64_constant(p, 0);
  if (low < 0 || high < 0 || count < 0 || sixty_four < 0 || zero < 0)
    return (c_value_t){-1, C_BAD, NULL};
  int is_small = lower_int128_unsigned_lt(p, count, sixty_four);
  int is_zero = emit_i64_compare(p, count, zero, NYIR_CMP_EQ);
  if (is_small < 0 || is_zero < 0)
    return (c_value_t){-1, C_BAD, NULL};
  size_t small_label = (size_t)p->next_label++;
  size_t zero_label = (size_t)p->next_label++;
  size_t large_label = (size_t)p->next_label++;
  size_t merge_label = (size_t)p->next_label++;
  size_t offset = 0;
  int result = lower_int128_allocate(p, value.agg, &offset);
  if (result < 0 || !emit_branch_if(p, is_small, (int64_t)small_label) ||
      !emit_branch(p, (int64_t)large_label) ||
      !emit_label(p, (int64_t)small_label) ||
      !emit_branch_if(p, is_zero, (int64_t)zero_label))
    return (c_value_t){-1, C_BAD, NULL};
  int complement = emit_i64_binary(p, NYIR_SUB_I64, sixty_four, count);
  int shifted_low = lower_int128_ushr(p, low, count);
  int shifted_high_bits = emit_value(
      p,
      (nyr_inst_t){.op = NYIR_SHL_I64, .dst = -1, .a = high, .b = complement});
  int low_result = emit_value(p, (nyr_inst_t){.op = NYIR_OR_I64,
                                              .dst = -1,
                                              .a = shifted_low,
                                              .b = shifted_high_bits});
  int high_result = lower_int128_ushr(p, high, count);
  if (complement < 0 || shifted_low < 0 || shifted_high_bits < 0 ||
      low_result < 0 || high_result < 0 ||
      lower_int128_store(p, result, 0, low_result) < 0 ||
      lower_int128_store(p, result, 8, high_result) < 0 ||
      !emit_branch(p, (int64_t)merge_label) ||
      !emit_label(p, (int64_t)zero_label) ||
      lower_int128_store(p, result, 0, low) < 0 ||
      lower_int128_store(p, result, 8, high) < 0 ||
      !emit_branch(p, (int64_t)merge_label) ||
      !emit_label(p, (int64_t)large_label))
    return (c_value_t){-1, C_BAD, NULL};
  int count_minus_sixty_four =
      emit_i64_binary(p, NYIR_SUB_I64, count, sixty_four);
  int large_low = lower_int128_ushr(p, high, count_minus_sixty_four);
  if (count_minus_sixty_four < 0 || large_low < 0 ||
      lower_int128_store(p, result, 0, large_low) < 0 ||
      lower_int128_store(p, result, 8, zero) < 0 ||
      !emit_label(p, (int64_t)merge_label))
    return (c_value_t){-1, C_BAD, NULL};
  return (c_value_t){result, C_AGG, value.agg};
}

static unsigned c_scalar_lvalue_flags(c_parser_t *p, const c_lvalue_t *lvalue,
                                      c_type_t type) {
  unsigned width = type == C_U8 || type == C_CHAR ? NYIR_INST_F_MEM_BYTE
                   : type == C_I32 || type == C_U32 || type == C_F32 ||
                           (p->abi == NYC_ABI_I386 && c_type_is_pointer(type))
                       ? NYIR_INST_F_MEM_I32
                       : 0;
  return width | lvalue->segment_flags;
}

static bool lower_store_scalar_lvalue(c_parser_t *p, const c_lvalue_t *lvalue,
                                      c_type_t type, int value) {
  if (type == C_I16 || type == C_U16)
    return lower_store_u16_field_seg(p, lvalue->address, value, type,
                                     lvalue->is_volatile,
                                     lvalue->segment_flags);
  return emit(
      p,
      (nyr_inst_t){.op = NYIR_STORE_I64,
                   .dst = -1,
                   .a = lvalue->address,
                   .c = value,
                   .effects = NYIR_EFFECT_WRITE_MEMORY | NYIR_EFFECT_MAY_TRAP |
                              (lvalue->is_volatile ? NYIR_EFFECT_VOLATILE : 0),
                   .flags = c_scalar_lvalue_flags(p, lvalue, type),
                   .scalar_class = c_type_is_float(type)
                                       ? c_float_scalar_class(type)
                                       : NYIR_SCALAR_I64_RAW},
      NULL);
}

static c_value_t parse_expr(c_parser_t *p, int min_precedence) {
  c_value_t left = parse_unary(p);
  if (left.value < 0 && !(min_precedence == 0 && tok_is(p, p->pos, ",") &&
                          (!p->err || !p->err[0])))
    return left;
  for (;;) {
    if (min_precedence <= 1 && is_assignment_operator(p, p->pos)) {
      ny_ctok_t assignment = *current(p);
      int lhs_value = left.value;
      const c_lvalue_t *target = find_lvalue(p, lhs_value);
      if (!target) {
        if (!getenv("NYC_KERNEL_COMPAT")) {
          fail(p, "assignment requires a modifiable C lvalue");
          return (c_value_t){-1, C_BAD, NULL};
        }
        /* NYC kernel compat: kernel swap/macros may produce lvalue forms NYC
           misclassifies; continue and let lowering/verification catch real
           errors. */
      }
      ++p->pos;
      c_value_t right = parse_expr(p, 1);
      if (right.value < 0)
        return right;
      target = find_lvalue(p, lhs_value);
      if (!target)
        return (c_value_t){-1, C_BAD, NULL};
      c_local_t *target_local =
          target->is_local ? find_local_slot(p, target->local_slot) : NULL;
      if (target->is_local && !target_local) {
        fail(p, "assignment target local is no longer in scope");
        return (c_value_t){-1, C_BAD, NULL};
      }
      if (target_local && target_local->is_const) {
        fail(p, "assignment requires a modifiable C lvalue");
        return (c_value_t){-1, C_BAD, NULL};
      }
      bool simple = ny_ctok_eq(assignment, "=");
      if (simple && left.type == C_AGG && right.type == C_AGG) {
        if (!target->agg || target->agg != right.agg || target->address < 0 ||
            target->is_volatile || !target->agg->complete ||
            (target->agg->size &&
             !lower_copy_struct(p, target->address, right.value,
                                target->agg->size))) {
          fail(p, "aggregate assignment requires matching nonvolatile "
                  "complete aggregate lvalues");
          return (c_value_t){-1, C_BAD, NULL};
        }
        left = right;
        p->last_value_null_pointer_constant = false;
        continue;
      }
      bool pointer_compound =
          c_type_is_arithmetic_pointer(left.type) &&
          c_type_is_integer(right.type) &&
          (ny_ctok_eq(assignment, "+=") || ny_ctok_eq(assignment, "-="));
      bool valid =
          left.type == right.type ||
          (c_type_is_integer(left.type) && c_type_is_integer(right.type)) ||
          (c_type_is_float(left.type) && c_type_is_float(right.type)) ||
          pointer_compound || c_pointer_compatible(left.type, right.type) ||
          (c_type_is_pointer(left.type) && c_type_is_integer(right.type) &&
           p->last_value_null_pointer_constant);
      if (!valid) {
        fail(p, "assignment value is incompatible with its C lvalue");
        return (c_value_t){-1, C_BAD, NULL};
      }
      unsigned memory_flags = c_scalar_lvalue_flags(p, target, left.type);
      if (!simple) {
        if (pointer_compound) {
          int previous =
              target_local
                  ? lower_local_load(p, target_local)
                  : emit_value(
                        p, (nyr_inst_t){.op = NYIR_LOAD_I64,
                                        .dst = -1,
                                        .a = target->address,
                                        .effects = NYIR_EFFECT_READ_MEMORY |
                                                   NYIR_EFFECT_MAY_TRAP |
                                                   (target->is_volatile
                                                        ? NYIR_EFFECT_VOLATILE
                                                        : 0),
                                        .flags = memory_flags,
                                        .scalar_class = NYIR_SCALAR_I64_RAW});
          right.value = previous < 0
                            ? -1
                            : lower_pointer_offset_for(
                                  p, left.type, target->agg, previous,
                                  right.value, ny_ctok_eq(assignment, "-="));
          if (right.value < 0)
            return (c_value_t){-1, C_BAD, NULL};
          right.type = left.type;
        } else if (c_type_is_pointer(left.type) ||
                   (!c_type_is_integer(left.type) &&
                    !c_type_is_float(left.type))) {
          fail(p, "compound assignment is unsupported for this C lvalue");
          return (c_value_t){-1, C_BAD, NULL};
        } else {
          int previous =
              target->bitfield
                  ? lower_load_bitfield(p, target->address, target->bitfield)
              : target_local ? lower_local_load(p, target_local)
              : left.type == C_I16 || left.type == C_U16
                  ? lower_load_u16_field_seg(p, target->address, left.type,
                                             target->is_volatile,
                                             target->segment_flags)
                  : emit_value(p, (nyr_inst_t){
                                      .op = NYIR_LOAD_I64,
                                      .dst = -1,
                                      .a = target->address,
                                      .effects = NYIR_EFFECT_READ_MEMORY |
                                                 NYIR_EFFECT_MAY_TRAP |
                                                 (target->is_volatile
                                                      ? NYIR_EFFECT_VOLATILE
                                                      : 0),
                                      .flags = memory_flags,
                                      .scalar_class =
                                          c_type_is_float(left.type)
                                              ? c_float_scalar_class(left.type)
                                              : NYIR_SCALAR_I64_RAW});
          if (previous >= 0)
            previous = normalize_integer(p, left.type, previous);
          nyr_op_t op = compound_assignment_ir(left.type, assignment);
          if (previous < 0 || op == NYIR_NOP)
            return (c_value_t){-1, C_BAD, NULL};
          right.value = emit_value(
              p,
              (nyr_inst_t){.op = op,
                           .dst = -1,
                           .a = previous,
                           .b = right.value,
                           .flags = c_i386_narrow32_operation(p, left.type, op)
                                        ? NYIR_INST_F_NARROW32
                                        : 0,
                           .scalar_class = c_type_is_float(left.type)
                                               ? c_float_scalar_class(left.type)
                                               : NYIR_SCALAR_I64_RAW});
          if (right.value < 0)
            return (c_value_t){-1, C_BAD, NULL};
        }
      }
      if (c_type_is_integer(left.type)) {
        right.value = normalize_integer(p, left.type, right.value);
        if (right.value < 0)
          return (c_value_t){-1, C_BAD, NULL};
      } else if (c_type_is_float(left.type) && c_type_is_float(right.type) &&
                 left.type != right.type) {
        right.value = emit_value(
            p, (nyr_inst_t){.op = left.type == C_F32 ? NYIR_F64_TO_F32
                                                     : NYIR_F32_TO_F64,
                            .dst = -1,
                            .a = right.value,
                            .scalar_class = c_float_scalar_class(left.type)});
        if (right.value < 0)
          return (c_value_t){-1, C_BAD, NULL};
      }
      if (target->bitfield) {
        right.value = lower_bitfield_value(p, target->bitfield, right.value);
        if (right.value < 0)
          return (c_value_t){-1, C_BAD, NULL};
      }
      bool stored =
          target->bitfield ? lower_store_bitfield(p, target->address,
                                                  target->bitfield, right.value)
          : target_local
              ? lower_local_store(p, target_local, right.value)
              : lower_store_scalar_lvalue(p, target, left.type, right.value);
      if (!stored)
        return (c_value_t){-1, C_BAD, NULL};
      left = (c_value_t){right.value, left.type, left.agg};
      p->last_value_null_pointer_constant = false;
      continue;
    }
    if (min_precedence == 0 && accept(p, ",")) {
      c_value_t right = parse_expr(p, 1);
      if (right.value < 0)
        return (c_value_t){-1, C_BAD, NULL};
      left = right;
      continue;
    }
    int prec = precedence(p);
    if (prec < min_precedence || prec == 0) {
      if (min_precedence > 1 || !accept(p, "?"))
        return left;
      if (left.value < 0 || left.type == C_BAD || p->next_local == SIZE_MAX ||
          p->next_label > INT_MAX - 3) {
        fail(p, "conditional expression requires scalar values and storage");
        return (c_value_t){-1, C_BAD, NULL};
      }
      bool left_is_null_pointer_constant = p->last_value_null_pointer_constant;
      bool left_pointee_const = p->last_value_pointee_const;
      int condition = lower_truth_value(p, left);
      if (condition < 0) {
        fail(p, "conditional expression condition is invalid");
        return (c_value_t){-1, C_BAD, NULL};
      }
      size_t result_slot = p->next_local++;
      size_t value_slot_start = result_slot;
      p->last_value_null_pointer_constant = false;
      size_t true_label = (size_t)p->next_label++;
      size_t false_label = (size_t)p->next_label++;
      size_t merge_label = (size_t)p->next_label++;
      if (!emit_branch_if(p, condition, (int64_t)true_label) ||
          !emit_branch(p, (int64_t)false_label) ||
          !emit_label(p, (int64_t)true_label))
        return (c_value_t){-1, C_BAD, NULL};
      c_value_t true_value;
      bool true_is_null_pointer_constant;
      bool true_pointee_const;
      bool omitted_true = accept(p, ":");
      if (omitted_true) {
        true_value = left;
        true_is_null_pointer_constant = left_is_null_pointer_constant;
        true_pointee_const = left_pointee_const;
      } else {
        true_value = parse_expr(p, 0);
        true_is_null_pointer_constant = p->last_value_null_pointer_constant;
        true_pointee_const = p->last_value_pointee_const;
      }
      bool true_is_void = true_value.value < 0 && true_value.type == C_BAD &&
                          (!p->err || !p->err_len || !p->err[0]);
      if ((!true_is_void &&
           (true_value.value < 0 || true_value.type == C_BAD)) ||
          (!omitted_true && !expect(p, ":"))) {
        fail(p, "conditional expression requires supported scalar arms");
        return (c_value_t){-1, C_BAD, NULL};
      }
      if (true_is_void) {
        if (!emit_branch(p, (int64_t)merge_label) ||
            !emit_label(p, (int64_t)false_label))
          return (c_value_t){-1, C_BAD, NULL};
        c_value_t false_value = parse_expr(p, 1);
        bool false_is_void = false_value.value < 0 &&
                             false_value.type == C_BAD &&
                             (!p->err || !p->err_len || !p->err[0]);
        if (!false_is_void &&
            (false_value.value < 0 || false_value.type == C_BAD)) {
          fail(p, "conditional expression requires supported scalar arms");
          return (c_value_t){-1, C_BAD, NULL};
        }
        if (!emit_branch(p, (int64_t)merge_label) ||
            !emit_label(p, (int64_t)merge_label))
          return (c_value_t){-1, C_BAD, NULL};
        p->next_local = value_slot_start;
        left = (c_value_t){-1, C_BAD, NULL};
        p->last_value_null_pointer_constant = false;
        p->last_value_pointee_const = false;
        continue;
      }
      nyr_scalar_class_t result_class =
          c_type_is_float(true_value.type)
              ? c_float_scalar_class(true_value.type)
              : NYIR_SCALAR_I64_RAW;
      size_t true_store_index = p->out->len;
      if (!emit(p,
                (nyr_inst_t){.op = NYIR_STORE_LOCAL,
                             .dst = -1,
                             .a = true_value.value,
                             .imm = (int64_t)result_slot,
                             .scalar_class = result_class},
                NULL) ||
          !emit_branch(p, (int64_t)merge_label) ||
          !emit_label(p, (int64_t)false_label))
        return (c_value_t){-1, C_BAD, NULL};
      c_value_t false_value = parse_expr(p, 1);
      bool false_is_null_pointer_constant = p->last_value_null_pointer_constant;
      bool false_pointee_const = p->last_value_pointee_const;
      bool false_is_void = false_value.value < 0 && false_value.type == C_BAD &&
                           (!p->err || !p->err_len || !p->err[0]);
      if (false_is_void) {
        p->out->data[true_store_index] =
            (nyr_inst_t){.op = NYIR_NOP, .dst = -1, .a = -1, .b = -1};
        if (!emit_branch(p, (int64_t)merge_label) ||
            !emit_label(p, (int64_t)merge_label))
          return (c_value_t){-1, C_BAD, NULL};
        p->next_local = value_slot_start;
        left = (c_value_t){-1, C_BAD, NULL};
        p->last_value_null_pointer_constant = false;
        p->last_value_pointee_const = false;
        continue;
      }
      /* The true arm is already stored in a slot of its own class, so a
         mixed floating/integer pair converts the false arm to the true
         arm's floating type (`c ? 1.5 : 0`). */
      if (c_type_is_float(true_value.type) && false_value.value >= 0 &&
          false_value.type != true_value.type &&
          (c_type_is_integer(false_value.type) ||
           c_type_is_float(false_value.type)))
        false_value = convert_numeric_float(p, false_value, true_value.type);
      c_type_t result_type = true_value.type;
      bool compatible = true_value.type == false_value.type ||
                        (c_type_is_integer(true_value.type) &&
                         c_type_is_integer(false_value.type));
      if (c_type_is_pointer(true_value.type) &&
          c_type_is_integer(false_value.type) &&
          false_is_null_pointer_constant) {
        result_type = true_value.type;
        compatible = true;
      } else if (c_type_is_pointer(false_value.type) &&
                 c_type_is_integer(true_value.type) &&
                 true_is_null_pointer_constant) {
        result_type = false_value.type;
        compatible = true;
      } else if (true_value.type == C_VOID_PTR &&
                 true_is_null_pointer_constant &&
                 c_type_is_object_pointer(false_value.type)) {
        result_type = false_value.type;
        compatible = true;
      } else if (false_value.type == C_VOID_PTR &&
                 false_is_null_pointer_constant &&
                 c_type_is_object_pointer(true_value.type)) {
        result_type = true_value.type;
        compatible = true;
      } else if (c_pointer_compatible(true_value.type, false_value.type)) {
        result_type =
            true_value.type == C_VOID_PTR || false_value.type == C_VOID_PTR
                ? C_VOID_PTR
                : true_value.type;
        compatible = true;
      }
      if (false_value.value < 0 || false_value.type == C_BAD || !compatible) {
        if (!getenv("NYC_KERNEL_COMPAT")) {
          fail(p,
               "conditional expression arms must have the same supported type");
          return (c_value_t){-1, C_BAD, NULL};
        }
        /* NYC kernel compat: trace macros and kernel conditionals can produce
           arm type combinations NYC doesn't unify; coerce to a safe type. */
        result_type =
            (c_type_is_pointer(true_value.type) ||
             c_type_is_pointer(false_value.type))
                ? (c_type_is_pointer(true_value.type) ? true_value.type
                                                      : false_value.type)
                : C_I64;
        if (result_type == C_BAD)
          result_type = C_I64;
        compatible = true;
      }
      if (!emit(p,
                (nyr_inst_t){.op = NYIR_STORE_LOCAL,
                             .dst = -1,
                             .a = false_value.value,
                             .imm = (int64_t)result_slot,
                             .scalar_class = result_class},
                NULL) ||
          !emit_branch(p, (int64_t)merge_label) ||
          !emit_label(p, (int64_t)merge_label))
        return (c_value_t){-1, C_BAD, NULL};
      int result = emit_value(p, (nyr_inst_t){.op = NYIR_LOAD_LOCAL,
                                              .dst = -1,
                                              .imm = (int64_t)result_slot,
                                              .scalar_class = result_class});
      const c_agg_type_t *result_agg = result_type == C_VOID_PTR ? NULL
                                       : result_type == true_value.type
                                           ? true_value.agg
                                           : false_value.agg;
      p->last_value_null_pointer_constant = c_type_is_integer(result_type) &&
                                            true_is_null_pointer_constant &&
                                            false_is_null_pointer_constant;
      p->last_value_pointee_const = c_type_is_pointer(result_type) &&
                                    (true_pointee_const || false_pointee_const);
      left = (c_value_t){result, result_type, result_agg};
      continue;
    }
    ny_ctok_t op = *current(p);
    ++p->pos;
    bool left_is_null_pointer_constant = p->last_value_null_pointer_constant;
    p->last_value_null_pointer_constant = false;
    bool logical_and = ny_ctok_eq(op, "&&");
    bool logical_or = ny_ctok_eq(op, "||");
    if (logical_and || logical_or) {
      if (left.value < 0 || left.type == C_BAD) {
        fail(p, "logical operators require scalar operands");
        return (c_value_t){-1, C_BAD, NULL};
      }
      int left_truth = lower_truth_value(p, left);
      if (left_truth < 0 || p->next_local == SIZE_MAX ||
          p->next_label > INT_MAX - 3) {
        fail(p, "logical expression temporary storage exhausted");
        return (c_value_t){-1, C_BAD, NULL};
      }
      size_t result_slot = p->next_local++;
      size_t rhs_label = (size_t)p->next_label++;
      size_t short_label = (size_t)p->next_label++;
      size_t merge_label = (size_t)p->next_label++;
      int short_value = logical_and ? 0 : 1;
      size_t true_target = logical_and ? rhs_label : short_label;
      size_t false_target = logical_and ? short_label : rhs_label;
      if (!emit_branch_if(p, left_truth, (int64_t)true_target) ||
          !emit_branch(p, (int64_t)false_target) ||
          !emit_label(p, (int64_t)short_label))
        return (c_value_t){-1, C_BAD, NULL};
      int short_constant = emit_i64_constant(p, short_value);
      if (short_constant < 0 ||
          !emit(p,
                (nyr_inst_t){.op = NYIR_STORE_LOCAL,
                             .dst = -1,
                             .a = short_constant,
                             .imm = (int64_t)result_slot,
                             .scalar_class = NYIR_SCALAR_I64_RAW},
                NULL) ||
          !emit_branch(p, (int64_t)merge_label) ||
          !emit_label(p, (int64_t)rhs_label))
        return (c_value_t){-1, C_BAD, NULL};
      c_value_t right = parse_expr(p, prec + 1);
      if (right.value < 0 || right.type == C_BAD) {
        fail(p, "logical operators require scalar operands");
        return (c_value_t){-1, C_BAD, NULL};
      }
      int right_truth = lower_boolean_value(p, right);
      if (right_truth < 0 ||
          !emit(p,
                (nyr_inst_t){.op = NYIR_STORE_LOCAL,
                             .dst = -1,
                             .a = right_truth,
                             .imm = (int64_t)result_slot,
                             .scalar_class = NYIR_SCALAR_I64_RAW},
                NULL) ||
          !emit_branch(p, (int64_t)merge_label) ||
          !emit_label(p, (int64_t)merge_label))
        return (c_value_t){-1, C_BAD, NULL};
      int result =
          emit_value(p, (nyr_inst_t){.op = NYIR_LOAD_LOCAL,
                                     .dst = -1,
                                     .imm = (int64_t)result_slot,
                                     .scalar_class = NYIR_SCALAR_I64_RAW});
      left = (c_value_t){result, C_I64, NULL};
      continue;
    }
    c_value_t right = parse_expr(p, prec + 1);
    if (left.value < 0 || right.value < 0)
      return (c_value_t){-1, C_BAD, NULL};
    bool left_function_pointer = left.type == C_FUNC_PTR;
    bool right_function_pointer = right.type == C_FUNC_PTR;
    bool left_pointer =
        c_type_is_arithmetic_pointer(left.type) || left_function_pointer;
    bool right_pointer =
        c_type_is_arithmetic_pointer(right.type) || right_function_pointer;
    bool left_opaque_pointer = left.type == C_VOID_PTR;
    bool right_opaque_pointer = right.type == C_VOID_PTR;
    if (left_pointer && right_pointer && ny_ctok_eq(op, "-")) {
      if (left.type != right.type) {
        fail(p, "pointer subtraction requires matching pointer types");
        return (c_value_t){-1, C_BAD, NULL};
      }
      int difference = emit_value(p, (nyr_inst_t){.op = NYIR_SUB_I64,
                                                  .dst = -1,
                                                  .a = left.value,
                                                  .b = right.value});
      int64_t scale = c_pointer_scale_for(p->abi, left.type, left.agg);
      if (difference < 0)
        return (c_value_t){-1, C_BAD, NULL};
      /* Array decay keeps the original rank; a pointer-to-array value
       * carries only its pointee shape. Either operand of a valid pointer
       * difference can supply the complete common element type. */
      const c_array_shape_t *shape = array_shape_for_value(p, left.value);
      if (!shape || (!shape->pointer_to_array && shape->rank < 2))
        shape = array_shape_for_value(p, right.value);
      if (shape && (shape->pointer_to_array || shape->rank > 1)) {
        size_t stride = shape->element_size;
        for (unsigned dim = shape->pointer_to_array ? 0 : 1; dim < shape->rank;
             ++dim) {
          if (!shape->extent[dim] || stride > INT64_MAX / shape->extent[dim]) {
            fail(p, "pointer difference requires a complete array row");
            return (c_value_t){-1, C_BAD, NULL};
          }
          stride *= shape->extent[dim];
        }
        scale = (int64_t)stride;
      }
      if (scale > 1) {
        int divisor = emit_value(
            p, (nyr_inst_t){.op = NYIR_CONST_I64, .dst = -1, .imm = scale});
        if (divisor < 0)
          return (c_value_t){-1, C_BAD, NULL};
        difference = emit_value(p, (nyr_inst_t){.op = NYIR_DIV_I64,
                                                .dst = -1,
                                                .a = difference,
                                                .b = divisor});
      }
      if (difference < 0)
        return (c_value_t){-1, C_BAD, NULL};
      left = (c_value_t){difference, C_I64, NULL};
      continue;
    }
    if ((left_pointer && c_type_is_integer(right.type) &&
         (ny_ctok_eq(op, "+") || ny_ctok_eq(op, "-"))) ||
        (right_pointer && c_type_is_integer(left.type) &&
         ny_ctok_eq(op, "+"))) {
      c_value_t pointer = left_pointer ? left : right;
      c_value_t index = left_pointer ? right : left;
      int address =
          pointer.type == C_FUNC_PTR
              ? emit_value(
                    p, (nyr_inst_t){.op = left_pointer && ny_ctok_eq(op, "-")
                                              ? NYIR_SUB_I64
                                              : NYIR_ADD_I64,
                                    .dst = -1,
                                    .a = pointer.value,
                                    .b = index.value,
                                    .scalar_class = NYIR_SCALAR_I64_RAW})
              : lower_pointer_offset_for(p, pointer.type, pointer.agg,
                                         pointer.value, index.value,
                                         left_pointer && ny_ctok_eq(op, "-"));
      if (address < 0)
        return (c_value_t){-1, C_BAD, NULL};
      left = (c_value_t){
          .value = address, .type = pointer.type, .agg = pointer.agg};
      continue;
    }
    if (left_opaque_pointer || right_opaque_pointer) {
      bool equality = ny_ctok_eq(op, "==") || ny_ctok_eq(op, "!=");
      bool relational = ny_ctok_eq(op, "<") || ny_ctok_eq(op, "<=") ||
                        ny_ctok_eq(op, ">") || ny_ctok_eq(op, ">=");
      bool null_comparison =
          (c_type_is_pointer(left.type) &&
           p->last_value_null_pointer_constant) ||
          (c_type_is_pointer(right.type) && left_is_null_pointer_constant);
      bool compatible =
          left.type == right.type ||
          c_pointer_compatible(left.type, right.type) ||
          ((left.type == C_FUNC_PTR && right.type == C_OPAQUE_PTR) ||
           (left.type == C_OPAQUE_PTR && right.type == C_FUNC_PTR));
      bool relational_compatible = compatible &&
                                   c_type_is_object_pointer(left.type) &&
                                   c_type_is_object_pointer(right.type);
      if ((!equality && !relational) ||
          (relational && !relational_compatible) ||
          (equality && !null_comparison && !compatible)) {
        fail(p, "incompatible opaque pointer comparison");
        return (c_value_t){-1, C_BAD, NULL};
      }
      nyr_cmp_t cmp = ny_ctok_eq(op, "==")   ? NYIR_CMP_EQ
                      : ny_ctok_eq(op, "!=") ? NYIR_CMP_NE
                      : ny_ctok_eq(op, "<")  ? NYIR_CMP_LT
                      : ny_ctok_eq(op, "<=") ? NYIR_CMP_LE
                      : ny_ctok_eq(op, ">")  ? NYIR_CMP_GT
                                             : NYIR_CMP_GE;
      int result = relational
                       ? emit_u64_compare(p, left.value, right.value, cmp)
                       : emit_i64_compare(p, left.value, right.value, cmp);
      left = (c_value_t){result, C_I64, NULL};
      continue;
    }
    if (c_type_is_pointer(left.type) || c_type_is_pointer(right.type)) {
      bool equality = ny_ctok_eq(op, "==") || ny_ctok_eq(op, "!=");
      bool relational = ny_ctok_eq(op, "<") || ny_ctok_eq(op, "<=") ||
                        ny_ctok_eq(op, ">") || ny_ctok_eq(op, ">=");
      bool null_comparison =
          (c_type_is_pointer(left.type) &&
           p->last_value_null_pointer_constant) ||
          (c_type_is_pointer(right.type) && left_is_null_pointer_constant);
      bool compatible =
          left.type == right.type ||
          c_pointer_compatible(left.type, right.type) ||
          ((left.type == C_FUNC_PTR && right.type == C_OPAQUE_PTR) ||
           (left.type == C_OPAQUE_PTR && right.type == C_FUNC_PTR));
      if ((!equality && !relational) ||
          (relational &&
           (!c_type_is_object_pointer(left.type) ||
            !c_type_is_object_pointer(right.type) || !compatible)) ||
          (equality && !null_comparison && !compatible)) {
        char message[128];
        snprintf(message, sizeof(message),
                 "pointer comparison '%.*s' unsupported (types %d/%d, "
                 "null %d/%d)",
                 (int)op.len, op.start, left.type, right.type,
                 left_is_null_pointer_constant,
                 p->last_value_null_pointer_constant);
        fail(p, message);
        return (c_value_t){-1, C_BAD, NULL};
      }
      if (relational) {
        int sign_bit = emit_value(
            p, (nyr_inst_t){.op = NYIR_CONST_I64, .dst = -1, .imm = INT64_MIN});
        if (sign_bit < 0)
          return (c_value_t){-1, C_BAD, NULL};
        left.value = emit_value(p, (nyr_inst_t){.op = NYIR_XOR_I64,
                                                .dst = -1,
                                                .a = left.value,
                                                .b = sign_bit});
        right.value = emit_value(p, (nyr_inst_t){.op = NYIR_XOR_I64,
                                                 .dst = -1,
                                                 .a = right.value,
                                                 .b = sign_bit});
        if (left.value < 0 || right.value < 0)
          return (c_value_t){-1, C_BAD, NULL};
      }
      nyr_cmp_t cmp = ny_ctok_eq(op, "==")   ? NYIR_CMP_EQ
                      : ny_ctok_eq(op, "!=") ? NYIR_CMP_NE
                      : ny_ctok_eq(op, "<")  ? NYIR_CMP_LT
                      : ny_ctok_eq(op, "<=") ? NYIR_CMP_LE
                      : ny_ctok_eq(op, ">")  ? NYIR_CMP_GT
                                             : NYIR_CMP_GE;
      int result = emit_value(p, (nyr_inst_t){.op = NYIR_CMP_I64,
                                              .dst = -1,
                                              .a = left.value,
                                              .b = right.value,
                                              .cmp = cmp});
      left = (c_value_t){result, C_I64, NULL};
      continue;
    }
    if (right.type == C_AGG && right.agg && right.agg->is_int128 &&
        c_type_is_integer(left.type) && ny_ctok_eq(op, "*")) {
      right = lower_int128_mul_u64(p, right, left);
      if (right.value < 0)
        return right;
      left = right;
      continue;
    }
    bool int128_comparison = ny_ctok_eq(op, "==") || ny_ctok_eq(op, "!=") ||
                             ny_ctok_eq(op, "<") || ny_ctok_eq(op, "<=") ||
                             ny_ctok_eq(op, ">") || ny_ctok_eq(op, ">=");
    if (int128_comparison &&
        ((left.type == C_AGG && left.agg && left.agg->is_int128) ||
         (right.type == C_AGG && right.agg && right.agg->is_int128))) {
      left = lower_int128_compare(p, left, right, op);
      if (left.value < 0)
        return left;
      continue;
    }
    if (left.type == C_AGG && left.agg && left.agg->is_int128) {
      if (ny_ctok_eq(op, "*") && (right.type == C_I32 || right.type == C_U32)) {
        left = lower_int128_mul_u32(p, left, right);
      } else if (ny_ctok_eq(op, "*") && c_type_is_integer(right.type)) {
        left = lower_int128_mul_u64(p, left, right);
      } else if (ny_ctok_eq(op, "+") && c_type_is_integer(right.type)) {
        left = lower_int128_add_u64(p, left, right);
      } else if (ny_ctok_eq(op, ">>") && c_type_is_integer(right.type)) {
        left = lower_int128_shift_right(p, left, right);
      } else if (ny_ctok_eq(op, "&") || ny_ctok_eq(op, "|") ||
                 ny_ctok_eq(op, "^")) {
        nyr_op_t bitwise_op = ny_ctok_eq(op, "&")   ? NYIR_AND_I64
                              : ny_ctok_eq(op, "|") ? NYIR_OR_I64
                                                    : NYIR_XOR_I64;
        left = lower_int128_bitwise(p, left, right, bitwise_op);
      } else {
        fail(p, "unsupported 128-bit integer operation");
        return (c_value_t){-1, C_BAD, NULL};
      }
      if (left.value < 0)
        return left;
      continue;
    }
    bool any_complex_operand =
        (left.type == C_AGG && left.agg && left.agg->scope_id == SIZE_MAX) ||
        (right.type == C_AGG && right.agg && right.agg->scope_id == SIZE_MAX);
    if (!any_complex_operand &&
        (c_type_is_float(left.type) || c_type_is_float(right.type))) {
      c_type_t common_type =
          left.type == C_F64 || right.type == C_F64 ? C_F64 : C_F32;
      left = convert_numeric_float(p, left, common_type);
      right = convert_numeric_float(p, right, common_type);
      if (left.value < 0 || right.value < 0)
        return (c_value_t){-1, C_BAD, NULL};
    }
    if (!any_complex_operand && left.type != right.type &&
        !(c_type_is_integer(left.type) && c_type_is_integer(right.type))) {
      fail(p, "mixed integer/double arithmetic is unsupported");
      return (c_value_t){-1, C_BAD, NULL};
    }
    if (c_type_is_integer(left.type) && c_type_is_integer(right.type) &&
        (left.type == C_U8 || right.type == C_U8 || left.type == C_CHAR ||
         right.type == C_CHAR)) {
      left.value = normalize_integer(p, left.type, left.value);
      right.value = normalize_integer(p, right.type, right.value);
      if (left.value < 0 || right.value < 0)
        return (c_value_t){-1, C_BAD, NULL};
      if (left.type == C_U8 || left.type == C_CHAR)
        left.type = C_I32;
      if (right.type == C_U8 || right.type == C_CHAR)
        right.type = C_I32;
    }
    bool shift_operation = ny_ctok_eq(op, "<<") || ny_ctok_eq(op, ">>");
    if (!shift_operation && c_type_is_integer(left.type) &&
        c_type_is_integer(right.type) &&
        (left.type == C_U32 || right.type == C_U32) && left.type != C_I64 &&
        right.type != C_I64 && left.type != C_U64 && right.type != C_U64) {
      left.value = normalize_u32(p, left.value);
      right.value = normalize_u32(p, right.value);
      if (left.value < 0 || right.value < 0)
        return (c_value_t){-1, C_BAD, NULL};
      left.type = right.type = C_U32;
    }
    /* A signed 64-bit operand represents every unsigned 32-bit value.
     * Shifts apply integer promotions independently and keep the left type. */
    bool unsigned_operation =
        shift_operation ? left.type == C_U64 || left.type == C_U32
                        : left.type == C_U64 || right.type == C_U64 ||
                              ((left.type == C_U32 || right.type == C_U32) &&
                               left.type != C_I64 && right.type != C_I64);
    bool signed_safe_unsigned_division = false;
    if (unsigned_operation && (ny_ctok_eq(op, "/") || ny_ctok_eq(op, "%"))) {
      int64_t left_constant = 0, right_constant = 0;
      signed_safe_unsigned_division =
          c_ir_nonnegative_constant(p, left.value, &left_constant) &&
          c_ir_nonnegative_constant(p, right.value, &right_constant) &&
          right_constant != 0;
    }
    if (unsigned_operation && (ny_ctok_eq(op, "/") || ny_ctok_eq(op, "%")) &&
        !signed_safe_unsigned_division) {
      int result =
          emit_value(p, (nyr_inst_t){.op = NYIR_INLINE_ASM,
                                     .dst = -1,
                                     .a = left.value,
                                     .b = right.value,
                                     .imm = ny_ctok_eq(op, "/") ? -21 : -22,
                                     .effects = NYIR_EFFECT_VOLATILE,
                                     .symbol = "unsigned-div"});
      if (result < 0)
        return (c_value_t){-1, C_BAD, NULL};
      c_type_t result_type =
          left.type == C_U64 || right.type == C_U64 ? C_U64 : C_U32;
      if (result_type == C_U32) {
        result = normalize_u32(p, result);
        if (result < 0)
          return (c_value_t){-1, C_BAD, NULL};
      }
      left = (c_value_t){result, result_type, NULL};
      continue;
    }
    if (unsigned_operation && ny_ctok_eq(op, ">>")) {
      if (left.type == C_U32) {
        left.value = normalize_u32(p, left.value);
        if (left.value < 0)
          return (c_value_t){-1, C_BAD, NULL};
      } else {
        int result =
            emit_value(p, (nyr_inst_t){.op = NYIR_INLINE_ASM,
                                       .dst = -1,
                                       .a = left.value,
                                       .b = right.value,
                                       .imm = -23,
                                       .effects = NYIR_EFFECT_NONE,
                                       .symbol = "unsigned-shift-right"});
        if (result < 0)
          return (c_value_t){-1, C_BAD, NULL};
        left = (c_value_t){result, C_U64, NULL};
        continue;
      }
    }
    if (unsigned_operation && (ny_ctok_eq(op, "/=") || ny_ctok_eq(op, "%="))) {
      fail(p, "unsigned division, remainder, and right shift are unsupported");
      return (c_value_t){-1, C_BAD, NULL};
    }
    bool left_complex =
        left.type == C_AGG && left.agg && left.agg->scope_id == SIZE_MAX;
    bool right_complex =
        right.type == C_AGG && right.agg && right.agg->scope_id == SIZE_MAX;
    if (left_complex || right_complex) {
      bool add = ny_ctok_eq(op, "+");
      bool subtract = ny_ctok_eq(op, "-");
      bool multiply = ny_ctok_eq(op, "*");
      bool divide = ny_ctok_eq(op, "/");
      bool equality = ny_ctok_eq(op, "==") || ny_ctok_eq(op, "!=");
      if (!add && !subtract && !multiply && !divide && !equality) {
        fail(p, "unsupported complex binary operator");
        return (c_value_t){-1, C_BAD, NULL};
      }
      bool widen = (left_complex && left.agg->size == 16) ||
                   (right_complex && right.agg->size == 16) ||
                   (!left_complex && left.type == C_F64) ||
                   (!right_complex && right.type == C_F64);
      if (!left_complex) {
        left = lower_scalar_to_complex(p, left, !widen);
        if (left.value < 0)
          return (c_value_t){-1, C_BAD, NULL};
        left_complex = true;
      }
      if (!right_complex) {
        right = lower_scalar_to_complex(p, right, !widen);
        if (right.value < 0)
          return (c_value_t){-1, C_BAD, NULL};
        right_complex = true;
      }
      if (left.agg->size != right.agg->size) {
        /*
         * Usual arithmetic conversions: promote the narrower complex operand
         * to the wider precision so a mixed `float _Complex`/`double _Complex`
         * expression keeps the wider result type.
         */
        bool left_narrow = left.agg->size < right.agg->size;
        c_value_t narrow = left_narrow ? left : right;
        bool source_single = narrow.agg->size == 8;
        bool target_single = !source_single;
        size_t narrow_lane = source_single ? 4 : 8;
        int real_source = source_single
                              ? lower_load_f32_field(p, narrow.value, false)
                              : lower_load_field(p, narrow.value, true, false);
        int imag_source_addr = lower_add_const(p, narrow.value, narrow_lane);
        int imag_source =
            imag_source_addr < 0 ? -1
            : source_single
                ? lower_load_f32_field(p, imag_source_addr, false)
                : lower_load_field(p, imag_source_addr, true, false);
        int real_value =
            real_source < 0
                ? -1
                : emit_value(p,
                             (nyr_inst_t){.op = source_single ? NYIR_F32_TO_F64
                                                              : NYIR_F64_TO_F32,
                                          .dst = -1,
                                          .a = real_source,
                                          .scalar_class =
                                              target_single ? NYIR_SCALAR_F32
                                                            : NYIR_SCALAR_F64});
        int imag_value =
            imag_source < 0
                ? -1
                : emit_value(p,
                             (nyr_inst_t){.op = source_single ? NYIR_F32_TO_F64
                                                              : NYIR_F64_TO_F32,
                                          .dst = -1,
                                          .a = imag_source,
                                          .scalar_class =
                                              target_single ? NYIR_SCALAR_F32
                                                            : NYIR_SCALAR_F64});
        size_t promote_offset = 0;
        if (real_value < 0 || imag_value < 0 ||
            !agg_area_alloc(p, target_single ? 8 : 16, &promote_offset))
          return (c_value_t){-1, C_BAD, NULL};
        int promote_base = p->agg_base;
        int imag_target =
            lower_add_const(p, promote_base, target_single ? 4 : 8);
        bool promoted =
            imag_target >= 0 &&
            (target_single
                 ? lower_store_f32_field(p, promote_base, real_value, false) &&
                       lower_store_f32_field(p, imag_target, imag_value, false)
                 : lower_store_field(p, promote_base, real_value, true,
                                     false) &&
                       lower_store_field(p, imag_target, imag_value, true,
                                         false));
        if (!promoted)
          return (c_value_t){-1, C_BAD, NULL};
        c_value_t promoted_value = {promote_base, C_AGG,
                                    target_single ? &lower_complex_value_f32
                                                  : &lower_complex_value_f64};
        if (left_narrow)
          left = promoted_value;
        else
          right = promoted_value;
      }
      if (left.agg->size == 32) {
        fail(p, "long double _Complex arithmetic is not supported yet");
        return (c_value_t){-1, C_BAD, NULL};
      }
      if (equality) {
        bool single_eq = left.agg->size == 8;
        size_t eq_lane = single_eq ? 4 : 8;
        int left_real = single_eq
                            ? lower_load_f32_field(p, left.value, false)
                            : lower_load_field(p, left.value, true, false);
        int right_real = single_eq
                             ? lower_load_f32_field(p, right.value, false)
                             : lower_load_field(p, right.value, true, false);
        int left_imag_addr = lower_add_const(p, left.value, eq_lane);
        int right_imag_addr = lower_add_const(p, right.value, eq_lane);
        int left_imag = left_imag_addr < 0 ? -1
                        : single_eq
                            ? lower_load_f32_field(p, left_imag_addr, false)
                            : lower_load_field(p, left_imag_addr, true, false);
        int right_imag =
            right_imag_addr < 0 ? -1
            : single_eq ? lower_load_f32_field(p, right_imag_addr, false)
                        : lower_load_field(p, right_imag_addr, true, false);
        if (left_real < 0 || left_imag < 0 || right_real < 0 || right_imag < 0)
          return (c_value_t){-1, C_BAD, NULL};
        bool equal_op = ny_ctok_eq(op, "==");
        nyr_op_t float_cmp = single_eq ? NYIR_CMP_F32 : NYIR_CMP_F64;
        int real_cmp = emit_value(
            p, (nyr_inst_t){.op = float_cmp,
                            .dst = -1,
                            .a = left_real,
                            .b = right_real,
                            .cmp = equal_op ? NYIR_CMP_EQ : NYIR_CMP_NE,
                            .scalar_class = NYIR_SCALAR_I64_RAW});
        int imag_cmp = emit_value(
            p, (nyr_inst_t){.op = float_cmp,
                            .dst = -1,
                            .a = left_imag,
                            .b = right_imag,
                            .cmp = equal_op ? NYIR_CMP_EQ : NYIR_CMP_NE,
                            .scalar_class = NYIR_SCALAR_I64_RAW});
        int combined =
            real_cmp < 0 || imag_cmp < 0
                ? -1
                : emit_value(p, (nyr_inst_t){.op = equal_op ? NYIR_AND_I64
                                                            : NYIR_OR_I64,
                                             .dst = -1,
                                             .a = real_cmp,
                                             .b = imag_cmp});
        return (c_value_t){combined, C_I64, NULL};
      }
      if ((multiply || divide) && left.agg->size == 32) {
        fail(p, "long double _Complex multiplication and division are not "
                "supported yet");
        return (c_value_t){-1, C_BAD, NULL};
      }
      if (multiply || divide) {
        bool single_call = left.agg->size == 8;
        size_t lane_offset = single_call ? 4 : 8;
        int left_real = single_call
                            ? lower_load_f32_field(p, left.value, false)
                            : lower_load_field(p, left.value, true, false);
        int left_imag =
            single_call
                ? lower_load_f32_field(
                      p, lower_add_const(p, left.value, lane_offset), false)
                : lower_load_field(p,
                                   lower_add_const(p, left.value, lane_offset),
                                   true, false);
        int right_real = single_call
                             ? lower_load_f32_field(p, right.value, false)
                             : lower_load_field(p, right.value, true, false);
        int right_imag =
            single_call
                ? lower_load_f32_field(
                      p, lower_add_const(p, right.value, lane_offset), false)
                : lower_load_field(p,
                                   lower_add_const(p, right.value, lane_offset),
                                   true, false);
        if (left_real < 0 || left_imag < 0 || right_real < 0 || right_imag < 0)
          return (c_value_t){-1, C_BAD, NULL};
        nyr_inst_t call = {
            .op = NYIR_CALL,
            .dst = -1,
            .a = left_real,
            .b = left_imag,
            .c = right_real,
            .d = right_imag,
            .e = -1,
            .f = -1,
            .imm = 4,
            .flags = NYIR_INST_F_EXTERN,
            .effects = NYIR_EFFECT_CALL,
            .symbol = single_call ? (multiply ? "__mulsc3" : "__divsc3")
                                  : (multiply ? "__muldc3" : "__divdc3"),
            .scalar_class = single_call ? NYIR_SCALAR_F32 : NYIR_SCALAR_F64,
            .ret_aggregate_size = single_call ? 8 : 16};
        call.ret_aggregate_classes[0] = NYIR_ARG_CLASS_SSE;
        call.ret_aggregate_classes[1] = single_call ? 0 : NYIR_ARG_CLASS_SSE;
        for (int argument = 0; argument < 4; ++argument)
          nyr_call_arg_class_set(call.arg_scalar_classes, argument,
                                 single_call ? NYIR_SCALAR_F32
                                             : NYIR_SCALAR_F64);
        int call_result = emit_value(p, call);
        if (call_result < 0) {
          fail(p, "complex arithmetic helper call failed");
          return (c_value_t){-1, C_BAD, NULL};
        }
        uint32_t packed = 0;
        if (!nyc_classify_aggregate(left.agg, p->abi, 0, &packed)) {
          fail(p, "complex aggregate is not classifiable by the C ABI");
          return (c_value_t){-1, C_BAD, NULL};
        }
        return lower_aggregate_call_result(p, call_result, left.agg, packed);
      }
      size_t offset = 0;
      if (!agg_area_alloc(p, left.agg->size, &offset)) {
        fail(p, "complex arithmetic storage allocation failed");
        return (c_value_t){-1, C_BAD, NULL};
      }
      int result = p->agg_base;
      bool single_precision = left.agg->size == 8;
      size_t half = left.agg->size / 2;
      for (size_t part = 0; part < 2; ++part) {
        int left_address = lower_add_const(p, left.value, part * half);
        int right_address = lower_add_const(p, right.value, part * half);
        int result_address = lower_add_const(p, result, part * half);
        int left_part = single_precision
                            ? lower_load_f32_field(p, left_address, false)
                            : lower_load_field(p, left_address, true, false);
        int right_part = single_precision
                             ? lower_load_f32_field(p, right_address, false)
                             : lower_load_field(p, right_address, true, false);
        if (left_address < 0 || right_address < 0 || result_address < 0 ||
            left_part < 0 || right_part < 0)
          return (c_value_t){-1, C_BAD, NULL};
        int part_value = emit_value(
            p, (nyr_inst_t){
                   .op = single_precision ? (add ? NYIR_ADD_F32 : NYIR_SUB_F32)
                                          : (add ? NYIR_ADD_F64 : NYIR_SUB_F64),
                   .dst = -1,
                   .a = left_part,
                   .b = right_part,
                   .scalar_class =
                       single_precision ? NYIR_SCALAR_F32 : NYIR_SCALAR_F64});
        if (part_value < 0)
          return (c_value_t){-1, C_BAD, NULL};
        bool stored =
            single_precision
                ? lower_store_f32_field(p, result_address, part_value, false)
                : lower_store_field(p, result_address, part_value, true, false);
        if (!stored)
          return (c_value_t){-1, C_BAD, NULL};
      }
      left = (c_value_t){result, C_AGG, left.agg};
      continue;
    }
    bool cmp = ny_ctok_eq(op, "==") || ny_ctok_eq(op, "!=") ||
               ny_ctok_eq(op, "<") || ny_ctok_eq(op, "<=") ||
               ny_ctok_eq(op, ">") || ny_ctok_eq(op, ">=");
    nyr_op_t ir_op = NYIR_NOP;
    nyr_cmp_t cmp_op = NYIR_CMP_EQ;
    if (ny_ctok_eq(op, "+"))
      ir_op = left.type == C_F32   ? NYIR_ADD_F32
              : left.type == C_F64 ? NYIR_ADD_F64
                                   : NYIR_ADD_I64;
    else if (ny_ctok_eq(op, "-"))
      ir_op = left.type == C_F32   ? NYIR_SUB_F32
              : left.type == C_F64 ? NYIR_SUB_F64
                                   : NYIR_SUB_I64;
    else if (ny_ctok_eq(op, "*"))
      ir_op = left.type == C_F32   ? NYIR_MUL_F32
              : left.type == C_F64 ? NYIR_MUL_F64
                                   : NYIR_MUL_I64;
    else if (ny_ctok_eq(op, "/"))
      ir_op = left.type == C_F32   ? NYIR_DIV_F32
              : left.type == C_F64 ? NYIR_DIV_F64
                                   : NYIR_DIV_I64;
    else if (ny_ctok_eq(op, "%"))
      ir_op = NYIR_MOD_I64;
    else if (ny_ctok_eq(op, "&"))
      ir_op = NYIR_AND_I64;
    else if (ny_ctok_eq(op, "|"))
      ir_op = NYIR_OR_I64;
    else if (ny_ctok_eq(op, "^"))
      ir_op = NYIR_XOR_I64;
    else if ((ny_ctok_eq(op, "<<") || ny_ctok_eq(op, ">>")) &&
             c_type_is_integer(left.type))
      ir_op = ny_ctok_eq(op, "<<") ? NYIR_SHL_I64 : NYIR_SAR_I64;
    else if (cmp) {
      bool relational = ny_ctok_eq(op, "<") || ny_ctok_eq(op, "<=") ||
                        ny_ctok_eq(op, ">") || ny_ctok_eq(op, ">=");
      /* Both unsigned int operands have already been zero-extended. Only
       * a full-width unsigned comparison needs to bias the sign bit. */
      if (relational && (left.type == C_U64 || right.type == C_U64)) {
        int sign_bit = emit_value(
            p, (nyr_inst_t){.op = NYIR_CONST_I64, .dst = -1, .imm = INT64_MIN});
        if (sign_bit < 0)
          return (c_value_t){-1, C_BAD, NULL};
        left.value = emit_value(p, (nyr_inst_t){.op = NYIR_XOR_I64,
                                                .dst = -1,
                                                .a = left.value,
                                                .b = sign_bit});
        right.value = emit_value(p, (nyr_inst_t){.op = NYIR_XOR_I64,
                                                 .dst = -1,
                                                 .a = right.value,
                                                 .b = sign_bit});
        if (left.value < 0 || right.value < 0)
          return (c_value_t){-1, C_BAD, NULL};
      }
      ir_op = left.type == C_F32   ? NYIR_CMP_F32
              : left.type == C_F64 ? NYIR_CMP_F64
                                   : NYIR_CMP_I64;
      cmp_op = ny_ctok_eq(op, "==")   ? NYIR_CMP_EQ
               : ny_ctok_eq(op, "!=") ? NYIR_CMP_NE
               : ny_ctok_eq(op, "<")  ? NYIR_CMP_LT
               : ny_ctok_eq(op, "<=") ? NYIR_CMP_LE
               : ny_ctok_eq(op, ">")  ? NYIR_CMP_GT
                                      : NYIR_CMP_GE;
    }
    if (ir_op == NYIR_NOP ||
        (c_type_is_float(left.type) && !cmp && ny_ctok_eq(op, "%"))) {
      fail(p, "unsupported binary operator for operand type");
      return (c_value_t){-1, C_BAD, NULL};
    }
    c_type_t result_type = left.type;
    if (shift_operation)
      result_type = left.type;
    else if (c_type_is_integer(left.type) && c_type_is_integer(right.type) &&
             left.type == C_U32 && right.type == C_U32)
      result_type = C_U32;
    else if (c_type_is_integer(left.type) && c_type_is_integer(right.type) &&
             (left.type == C_U64 || right.type == C_U64))
      result_type = C_U64;
    else if (c_type_is_integer(left.type) && c_type_is_integer(right.type) &&
             (left.type == C_I64 || right.type == C_I64))
      result_type = C_I64;
    bool narrow32 = !cmp && c_i386_narrow32_operation(p, result_type, ir_op);
    int result = emit_value(
        p, (nyr_inst_t){.op = ir_op,
                        .dst = -1,
                        .a = left.value,
                        .b = right.value,
                        .flags = narrow32 ? NYIR_INST_F_NARROW32 : 0,
                        .cmp = cmp_op,
                        .scalar_class = cmp ? NYIR_SCALAR_I64_RAW
                                        : c_type_is_float(left.type)
                                            ? c_float_scalar_class(left.type)
                                            : NYIR_SCALAR_I64_RAW});
    if (!cmp && result_type == C_I32 && narrow32)
      result = normalize_i32(p, result);
    else if (!cmp && result_type == C_U32)
      result = normalize_u32(p, result);
    left = (c_value_t){result, cmp ? C_I64 : result_type, NULL};
  }
}

static int lower_increment(c_parser_t *p, c_local_t *local, bool decrement,
                           bool prefix, int previous_value) {
  bool pointer = local && c_type_is_arithmetic_pointer(local->type);
  if (!local || local->is_const) {
    fail(p, "increment and decrement require a mutable scalar or "
            "pointer local");
    return -1;
  }
  if (previous_value < 0)
    previous_value = lower_local_load(p, local);
  if (previous_value < 0)
    return -1;
  int one;
  if (c_type_is_float(local->type)) {
    if (local->type == C_F32) {
      uint32_t bits = UINT32_C(0x3f800000);
      one = emit_value(p, (nyr_inst_t){.op = NYIR_CONST_F32,
                                       .dst = -1,
                                       .imm = (int64_t)bits,
                                       .scalar_class = NYIR_SCALAR_F32});
    } else {
      double value = 1.0;
      int64_t bits = 0;
      memcpy(&bits, &value, sizeof(bits));
      one = emit_value(p, (nyr_inst_t){.op = NYIR_CONST_F64,
                                       .dst = -1,
                                       .imm = bits,
                                       .scalar_class = NYIR_SCALAR_F64});
    }
  } else {
    one = emit_i64_constant(p, 1);
  }
  if (one < 0)
    return -1;
  int updated =
      pointer
          ? (local->type == C_AGG_PTR && local->agg
                 ? lower_pointer_offset_sized(p, (int64_t)local->agg->size,
                                              previous_value, one, decrement)
                 : lower_pointer_offset(p, local->type, previous_value, one,
                                        decrement))
          : emit_value(
                p, (nyr_inst_t){
                       .op = local->type == C_F32
                                 ? (decrement ? NYIR_SUB_F32 : NYIR_ADD_F32)
                             : local->type == C_F64
                                 ? (decrement ? NYIR_SUB_F64 : NYIR_ADD_F64)
                                 : (decrement ? NYIR_SUB_I64 : NYIR_ADD_I64),
                       .dst = -1,
                       .a = previous_value,
                       .b = one,
                       .scalar_class = c_type_is_float(local->type)
                                           ? c_float_scalar_class(local->type)
                                           : NYIR_SCALAR_I64_RAW});
  if (updated >= 0 && local->is_bool && c_type_is_integer(local->type))
    updated = lower_bool_value(p, updated);
  else if (updated >= 0 && c_type_is_integer(local->type))
    updated = normalize_integer(p, local->type, updated);
  if (updated < 0 || !lower_local_store(p, local, updated))
    return -1;
  return prefix ? updated : previous_value;
}

static int lower_increment_lvalue(c_parser_t *p, const c_lvalue_t *lvalue,
                                  bool decrement, bool prefix,
                                  int previous_value) {
  if (!lvalue || lvalue->address < 0) {
    fail(p, "increment requires a modifiable lvalue");
    return -1;
  }
  if (lvalue->is_volatile && lvalue->bitfield) {
    fail(p, "increment of volatile bit-fields is unsupported");
    return -1;
  }
  if (c_type_is_float(lvalue->type)) {
    bool is_f32 = lvalue->type == C_F32;
    int one_float = emit_value(
        p, (nyr_inst_t){.op = is_f32 ? NYIR_CONST_F32 : NYIR_CONST_F64,
                        .dst = -1,
                        .imm = is_f32 ? INT64_C(0x3f800000)
                                      : INT64_C(0x3ff0000000000000),
                        .scalar_class = c_float_scalar_class(lvalue->type)});
    int updated =
        one_float < 0
            ? -1
            : emit_value(
                  p,
                  (nyr_inst_t){
                      .op = is_f32 ? (decrement ? NYIR_SUB_F32 : NYIR_ADD_F32)
                                   : (decrement ? NYIR_SUB_F64 : NYIR_ADD_F64),
                      .dst = -1,
                      .a = previous_value,
                      .b = one_float,
                      .scalar_class = c_float_scalar_class(lvalue->type)});
    if (updated < 0 ||
        !lower_store_scalar_lvalue(p, lvalue, lvalue->type, updated))
      return -1;
    return prefix ? updated : previous_value;
  }
  int one = emit_i64_constant(p, 1);
  if (one < 0)
    return -1;
  int updated;
  if (c_type_is_arithmetic_pointer(lvalue->type)) {
    updated = lower_pointer_offset_for(p, lvalue->type, lvalue->agg,
                                       previous_value, one, decrement);
    if (updated < 0)
      return -1;
  } else {
    if (!c_type_is_integer(lvalue->type)) {
      fail(p, "postfix increment requires an integer or pointer lvalue");
      return -1;
    }
    updated = emit_value(
        p, (nyr_inst_t){.op = decrement ? NYIR_SUB_I64 : NYIR_ADD_I64,
                        .dst = -1,
                        .a = previous_value,
                        .b = one,
                        .scalar_class = NYIR_SCALAR_I64_RAW});
    if (updated < 0)
      return -1;
    if (c_type_is_integer(lvalue->type))
      updated = normalize_integer(p, lvalue->type, updated);
    if (updated < 0)
      return -1;
    if (lvalue->bitfield) {
      updated = lower_bitfield_value(p, lvalue->bitfield, updated);
      if (updated < 0 ||
          !lower_store_bitfield(p, lvalue->address, lvalue->bitfield, updated))
        return -1;
      return prefix ? updated : previous_value;
    }
  }
  if (!lower_store_scalar_lvalue(p, lvalue, lvalue->type, updated))
    return -1;
  return prefix ? updated : previous_value;
}
