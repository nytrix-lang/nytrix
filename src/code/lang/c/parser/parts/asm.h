/*
 * Internal file-scope assembly section helpers, included by parse.c.
 */
#ifndef NYTRIX_C_PARSE_ASM_BASE_H
#define NYTRIX_C_PARSE_ASM_BASE_H

static bool append_asm_bytes(unsigned char **bytes, size_t *length,
                             size_t *capacity, const void *source,
                             size_t count) {
  if (count >= SIZE_MAX - *length)
    return false;
  size_t required = *length + count;
  size_t storage_required = required + 1;
  if (storage_required > *capacity) {
    size_t next = *capacity ? *capacity : 16;
    while (next < storage_required) {
      if (next > SIZE_MAX / 2) {
        next = storage_required;
        break;
      }
      next *= 2;
    }
    unsigned char *grown = realloc(*bytes, next);
    if (!grown)
      return false;
    *bytes = grown;
    *capacity = next;
  }
  if (count)
    memcpy(*bytes + *length, source, count);
  *length = required;
  (*bytes)[required] = '\0';
  return true;
}

static void unescape_inline_asm_percents(unsigned char *assembly,
                                         size_t *length) {
  size_t output = 0;
  for (size_t input = 0; input < *length; ++input) {
    assembly[output++] = assembly[input];
    if (assembly[input] == '%' && input + 1 < *length &&
        assembly[input + 1] == '%')
      ++input;
  }
  *length = output;
  assembly[output] = '\0';
}

static char *trim_asm_space(char *text) {
  while (isspace((unsigned char)*text))
    ++text;
  size_t length = strlen(text);
  while (length && isspace((unsigned char)text[length - 1]))
    text[--length] = '\0';
  return text;
}

static bool parse_asm_section(char *argument, char **name, uint64_t *flags,
                              size_t *entsize) {
  char *comma = strchr(argument, ',');
  char *section = argument;
  char *flag_text = NULL;
  if (comma) {
    *comma = '\0';
    flag_text = trim_asm_space(comma + 1);
  }
  section = trim_asm_space(section);
  size_t section_len = strlen(section);
  if (section_len >= 2 && section[0] == '"' &&
      section[section_len - 1] == '"') {
    section[section_len - 1] = '\0';
    ++section;
  }
  if (flag_text && *flag_text && flag_text[0] != '"')
    return false;
  char *suffix = "";
  if (!flag_text || !*flag_text) {
    flag_text = "";
  } else {
    char *flags_end = strchr(flag_text + 1, '"');
    if (!flags_end)
      return false;
    suffix = trim_asm_space(flags_end + 1);
    *flags_end = '\0';
    ++flag_text;
  }
  if (!*section)
    return false;
  uint64_t section_flags = 0;
  for (const char *at = flag_text; *at; ++at) {
    if (*at == 'a')
      section_flags |= 2u;
    else if (*at == 'w')
      section_flags |= 1u;
    else if (*at == 'x')
      section_flags |= 4u;
    else if (*at == 'M')
      section_flags |= 0x10u;
    else if (*at == 'S')
      section_flags |= 0x20u;
    else if (*at == 'G')
      section_flags |= 0x200u;
    else if (*at == 'T')
      section_flags |= 0x400u;
    else
      return false;
  }
  if (!comma) {
    if (strcmp(section, ".text") == 0 || strncmp(section, ".text.", 6) == 0 ||
        strcmp(section, ".entry.text") == 0)
      section_flags = 2u | 4u;
    else if (strcmp(section, ".rodata") == 0 ||
             strncmp(section, ".rodata.", 8) == 0)
      section_flags = 2u;
  }
  size_t section_entsize = 0;
  if (*suffix) {
    if (*suffix++ != ',')
      return false;
    suffix = trim_asm_space(suffix);
    if (strncmp(suffix, "@progbits", 9) == 0)
      suffix = trim_asm_space(suffix + 9);
    if (*suffix) {
      if (*suffix++ != ',')
        return false;
      suffix = trim_asm_space(suffix);
      char *endptr = NULL;
      unsigned long parsed = strtoul(suffix, &endptr, 0);
      if (!endptr || *trim_asm_space(endptr) || parsed > 4096)
        return false;
      section_entsize = (size_t)parsed;
    }
  }
  *name = strdup(section);
  if (!*name)
    return false;
  *flags = section_flags;
  *entsize = section_entsize;
  return true;
}

typedef struct {
  char *name;
  size_t offset;
  bool function_symbol;
  bool is_local;
} c_file_asm_symbol_t;

typedef struct {
  char *name;
  c_file_asm_symbol_t *symbols;
  size_t symbol_count;
  size_t symbol_capacity;
  uint64_t flags;
  size_t alignment;
  size_t entsize;
  unsigned char *bytes;
  size_t length;
  size_t capacity;
  c_global_reloc_t *relocs;
  size_t reloc_count;
  size_t reloc_capacity;
  unsigned local_label;
  size_t local_label_offset;
  size_t local_label_order;
  bool have_local_label;
} c_file_asm_section_t;

static inline bool asm_section_append(c_file_asm_section_t *section,
                                      const void *bytes, size_t length) {
  return append_asm_bytes(&section->bytes, &section->length, &section->capacity,
                          bytes, length);
}

static bool asm_section_add_symbol(c_file_asm_section_t *section,
                                   const char *name, size_t offset) {
  for (size_t i = 0; i < section->symbol_count; ++i) {
    if (strcmp(section->symbols[i].name, name) == 0)
      return section->symbols[i].offset == offset;
  }
  if (section->symbol_count &&
      offset < section->symbols[section->symbol_count - 1].offset)
    return false;
  if (section->symbol_count == section->symbol_capacity) {
    size_t next = section->symbol_capacity ? section->symbol_capacity * 2 : 4;
    if (next > SIZE_MAX / sizeof(*section->symbols))
      return false;
    c_file_asm_symbol_t *grown =
        realloc(section->symbols, next * sizeof(*grown));
    if (!grown)
      return false;
    section->symbols = grown;
    section->symbol_capacity = next;
  }
  char *copy = strdup(name);
  if (!copy)
    return false;
  section->symbols[section->symbol_count++] =
      (c_file_asm_symbol_t){.name = copy, .offset = offset};
  return true;
}

static c_file_asm_symbol_t *
asm_section_find_symbol(c_file_asm_section_t *section, const char *name) {
  for (size_t i = 0; i < section->symbol_count; ++i)
    if (strcmp(section->symbols[i].name, name) == 0)
      return &section->symbols[i];
  return NULL;
}
#endif

#ifdef NYTRIX_C_PARSE_INLINE_ASM_HELPERS
#ifndef NYTRIX_C_PARSE_INLINE_ASM_HELPERS_H
#define NYTRIX_C_PARSE_INLINE_ASM_HELPERS_H

/*
 * GNU inline-assembly operand and template helpers, included by parse.c after
 * their parser dependencies have been defined.
 */
static const c_lvalue_t *find_asm_memory_lvalue(c_parser_t *p, int value) {
  const c_lvalue_t *lvalue = find_lvalue(p, value);
  if (!lvalue || lvalue->bitfield ||
      (lvalue->type != C_AGG && !discard_lvalue_read(p, lvalue)))
    return NULL;
  if (lvalue->is_local && lvalue->address < 0) {
    size_t index = (size_t)(lvalue - p->lvalues);
    c_local_t *local = find_local_slot(p, lvalue->local_slot);
    if (!local)
      return NULL;
    local->address_taken = true;
    int address =
        emit_value(p, (nyr_inst_t){.op = NYIR_ADDR_LOCAL,
                                   .dst = -1,
                                   .imm = (int64_t)local->slot,
                                   .scalar_class = NYIR_SCALAR_I64_RAW});
    if (address < 0)
      return NULL;
    p->lvalues[index].address = address;
    return &p->lvalues[index];
  }
  return lvalue;
}

static size_t inline_asm_immediate_placeholder(const unsigned char *assembly,
                                               size_t length, size_t at,
                                               size_t *operand_index) {
  if (at + 1 >= length || assembly[at] != '%')
    return 0;
  size_t digit = at + 1;
  if (assembly[digit] == 'c')
    ++digit;
  if (digit >= length || !isdigit(assembly[digit]))
    return 0;
  size_t index = 0;
  do {
    unsigned value = (unsigned)(assembly[digit] - '0');
    if (index > (SIZE_MAX - value) / 10)
      return 0;
    index = index * 10 + value;
    ++digit;
  } while (digit < length && isdigit(assembly[digit]));
  if (operand_index)
    *operand_index = index;
  return digit - at;
}

static bool substitute_inline_asm_immediate(unsigned char **assembly,
                                            size_t *assembly_len,
                                            size_t *assembly_cap,
                                            const char *const *immediates,
                                            size_t immediate_count,
                                            size_t memory_operand_index) {
  unsigned char *expanded = NULL;
  size_t expanded_len = 0;
  size_t expanded_cap = 0;
  bool replaced = false;
  for (size_t i = 0; i < *assembly_len; ++i) {
    size_t operand_index = 0;
    size_t placeholder_len = inline_asm_immediate_placeholder(
        *assembly, *assembly_len, i, &operand_index);
    if ((*assembly)[i] == '%' && i + 1 < *assembly_len &&
        (*assembly)[i + 1] == '%') {
      if (!append_asm_bytes(&expanded, &expanded_len, &expanded_cap,
                            *assembly + i, 2)) {
        free(expanded);
        return false;
      }
      ++i;
    } else if (placeholder_len) {
      bool memory_operand =
          operand_index == memory_operand_index && (*assembly)[i + 1] != 'c';
      const char *replacement = memory_operand ? "%P1"
                                : operand_index < immediate_count
                                    ? immediates[operand_index]
                                    : NULL;
      if (!replacement ||
          !append_asm_bytes(&expanded, &expanded_len, &expanded_cap,
                            replacement, strlen(replacement))) {
        free(expanded);
        return false;
      }
      i += placeholder_len - 1;
      replaced = true;
    } else if (!append_asm_bytes(&expanded, &expanded_len, &expanded_cap,
                                 *assembly + i, 1)) {
      free(expanded);
      return false;
    }
  }
  if (!replaced) {
    free(expanded);
    return false;
  }
  free(*assembly);
  *assembly = expanded;
  *assembly_len = expanded_len;
  *assembly_cap = expanded_cap;
  return true;
}

static int inline_asm_xchg_width(const unsigned char *assembly, size_t length) {
  static const char *const templates[] = {"xchgb %b0, %1", "xchgw %w0, %1",
                                          "xchgl %0, %1", "xchgq %q0, %1"};
  for (size_t i = 0; i < sizeof(templates) / sizeof(templates[0]); ++i)
    if (length == strlen(templates[i]) &&
        memcmp(assembly, templates[i], length) == 0)
      return (int)i;
  return -1;
}

static bool inline_asm_template_equals(const unsigned char *assembly,
                                       size_t length, const char *template) {
  while (length && isspace((unsigned char)*assembly)) {
    ++assembly;
    --length;
  }
  while (length && isspace(assembly[length - 1]))
    --length;
  size_t template_len = strlen(template);
  return length == template_len &&
         memcmp(assembly, template, template_len) == 0;
}

static bool
inline_asm_template_ignoring_whitespace(const unsigned char *assembly,
                                        size_t length, const char *template) {
  size_t template_len = strlen(template);
  size_t assembly_at = 0;
  size_t template_at = 0;
  for (;;) {
    while (assembly_at < length &&
           isspace((unsigned char)assembly[assembly_at]))
      ++assembly_at;
    while (template_at < template_len &&
           isspace((unsigned char)template[template_at]))
      ++template_at;
    if (assembly_at == length || template_at == template_len)
      return assembly_at == length && template_at == template_len;
    if (assembly[assembly_at++] != (unsigned char)template[template_at++])
      return false;
  }
}

static int inline_asm_xadd_width(const unsigned char *assembly, size_t length) {
  static const char *const templates[] = {
      "lock xaddb %b0, %1", "lock xaddw %w0, %1", "lock xaddl %0, %1",
      "lock xaddq %q0, %1"};
  for (size_t i = 0; i < sizeof(templates) / sizeof(*templates); ++i)
    if (inline_asm_template_equals(assembly, length, templates[i]))
      return (int)i;
  return -1;
}

static bool substitute_inline_asm_register_memory(unsigned char **assembly,
                                                  size_t *assembly_len,
                                                  size_t *assembly_cap) {
  unsigned char *expanded = NULL;
  size_t expanded_len = 0;
  size_t expanded_cap = 0;
  unsigned register_operands = 0;
  unsigned memory_operands = 0;
  for (size_t i = 0; i < *assembly_len;) {
    const char *replacement = NULL;
    size_t consumed = 0;
    if (i + 2 < *assembly_len && (*assembly)[i] == '%' &&
        ((*assembly)[i + 1] == 'b' || (*assembly)[i + 1] == 'w' ||
         (*assembly)[i + 1] == 'q') &&
        (*assembly)[i + 2] == '0') {
      replacement = (*assembly)[i + 1] == 'b'   ? "%dl"
                    : (*assembly)[i + 1] == 'w' ? "%dx"
                                                : "%rdx";
      consumed = 3;
      ++register_operands;
    } else if (i + 1 < *assembly_len && (*assembly)[i] == '%' &&
               (*assembly)[i + 1] == '0') {
      replacement = "%edx";
      consumed = 2;
      ++register_operands;
    } else if (i + 1 < *assembly_len && (*assembly)[i] == '%' &&
               (*assembly)[i + 1] == '1') {
      replacement = "%P1";
      consumed = 2;
      ++memory_operands;
    }
    if (replacement) {
      if (!append_asm_bytes(&expanded, &expanded_len, &expanded_cap,
                            (const unsigned char *)replacement,
                            strlen(replacement)))
        goto fail;
      i += consumed;
    } else {
      if (!append_asm_bytes(&expanded, &expanded_len, &expanded_cap,
                            *assembly + i, 1))
        goto fail;
      ++i;
    }
  }
  if (register_operands != 1 || memory_operands != 1)
    goto fail;
  free(*assembly);
  *assembly = expanded;
  *assembly_len = expanded_len;
  *assembly_cap = expanded_cap;
  return true;

fail:
  free(expanded);
  return false;
}

/* Match NUL-terminated assembly fragments without repeating literal lengths. */
#define INLINE_ASM_HAS(text)                                                   \
  (assembly_len >= sizeof(text) - 1 &&                                         \
   memmem(assembly, assembly_len, text, sizeof(text) - 1))

#endif
#endif

#ifndef NYTRIX_C_PARSE_ASM_TAIL_H
#define NYTRIX_C_PARSE_ASM_TAIL_H

static void asm_section_dispose(c_file_asm_section_t *section) {
  free(section->name);
  free(section->bytes);
  for (size_t i = 0; i < section->reloc_count; ++i)
    free(section->relocs[i].owned_target);
  free(section->relocs);
  for (size_t i = 0; i < section->symbol_count; ++i)
    free(section->symbols[i].name);
  free(section->symbols);
}

static bool asm_standard_section_flags(const char *name, uint64_t *flags) {
  if (strcmp(name, ".text") == 0)
    *flags = 2u | 4u;
  else if (strcmp(name, ".rodata") == 0)
    *flags = 2u;
  else if (strcmp(name, ".data") == 0 || strcmp(name, ".bss") == 0)
    *flags = 2u | 1u;
  else
    return false;
  return true;
}

static char *asm_file_section_name(c_parser_t *p, const char *base_name) {
  char name[80];
  snprintf(name, sizeof(name), "%s.nyc_file_asm_%zu", base_name,
           p->asm_section_count++);
  return strdup(name);
}

static bool asm_section_get(c_file_asm_section_t **sections,
                            size_t *section_count, size_t *section_capacity,
                            char *name, uint64_t flags, size_t entsize,
                            size_t *index) {
  for (size_t i = 0; i < *section_count; ++i) {
    if (strcmp((*sections)[i].name, name) != 0)
      continue;
    free(name);
    if ((*sections)[i].flags != flags || (*sections)[i].entsize != entsize)
      return false;
    *index = i;
    return true;
  }
  if (*section_count >= 32) {
    free(name);
    return false;
  }
  if (*section_count == *section_capacity) {
    size_t next = *section_capacity ? *section_capacity * 2 : 4;
    if (next > 32)
      next = 32;
    c_file_asm_section_t *grown = realloc(*sections, next * sizeof(**sections));
    if (!grown) {
      free(name);
      return false;
    }
    *sections = grown;
    *section_capacity = next;
  }
  size_t at = (*section_count)++;
  (*sections)[at] = (c_file_asm_section_t){
      .name = name, .flags = flags, .alignment = 1, .entsize = entsize};
  *index = at;
  return true;
}

static bool asm_add_reloc(c_file_asm_section_t *section, const char *symbol,
                          size_t symbol_len, int64_t addend, size_t width,
                          uint8_t type) {
  if (width != sizeof(uint32_t) && width != sizeof(uint64_t))
    return false;
  if (section->reloc_count == section->reloc_capacity) {
    size_t next = section->reloc_capacity ? section->reloc_capacity * 2 : 4;
    if (next > SIZE_MAX / sizeof(*section->relocs))
      return false;
    c_global_reloc_t *grown = realloc(section->relocs, next * sizeof(*grown));
    if (!grown)
      return false;
    section->relocs = grown;
    section->reloc_capacity = next;
  }
  static const uint64_t zero = 0;
  size_t offset = section->length;
  if (!asm_section_append(section, &zero, width))
    return false;
  section->relocs[section->reloc_count++] = (c_global_reloc_t){
      .offset = offset,
      .target = {.kind = NY_CTOK_IDENT, .start = symbol, .len = symbol_len},
      .addend = addend,
      .width = width,
      .type = type};
  return true;
}

static bool asm_add_pc32_reloc(c_file_asm_section_t *section,
                               const char *symbol, size_t symbol_len,
                               int64_t addend) {
  return asm_add_reloc(section, symbol, symbol_len, addend, sizeof(uint32_t),
                       NYC_DATA_RELOC_PC32);
}

static bool asm_next_list_item(char **at, char **item) {
  if (!*at)
    return false;
  *item = *at;
  char *comma = strchr(*at, ',');
  if (comma) {
    *comma = '\0';
    *at = comma + 1;
  } else {
    *at = NULL;
  }
  return true;
}

static bool asm_append_u32(c_file_asm_section_t *section, uint32_t value) {
  unsigned char bytes[4] = {(unsigned char)value, (unsigned char)(value >> 8),
                            (unsigned char)(value >> 16),
                            (unsigned char)(value >> 24)};
  return asm_section_append(section, bytes, sizeof(bytes));
}

static bool asm_emit_long_value(c_file_asm_section_t *section,
                                c_file_asm_section_t *sections,
                                size_t section_count, size_t asm_token,
                                char *expression, char *assembly,
                                char *assembly_work, size_t assembly_len) {
  char *item = trim_asm_space(expression);
  char *numeric_item = item;
  bool parenthesized = numeric_item[0] == '(';
  if (parenthesized) {
    size_t item_len = strlen(numeric_item);
    if (item_len < 3 || numeric_item[item_len - 1] != ')')
      numeric_item = NULL;
    else {
      numeric_item[item_len - 1] = '\0';
      numeric_item = trim_asm_space(numeric_item + 1);
    }
  }
  if (numeric_item && numeric_item[0] == '~') {
    char *operand = trim_asm_space(numeric_item + 1);
    char *operand_end = NULL;
    long long value = strtoll(operand, &operand_end, 0);
    if (operand_end != operand && !*trim_asm_space(operand_end)) {
      uint32_t encoded = (uint32_t)~(uint64_t)value;
      return asm_append_u32(section, encoded);
    }
  }
  char *endptr = NULL;
  long long number = numeric_item ? strtoll(numeric_item, &endptr, 0) : 0;
  if (numeric_item && endptr != numeric_item && !*trim_asm_space(endptr)) {
    if (number < INT32_MIN || number > (long long)UINT32_MAX)
      return false;
    uint32_t value = (uint32_t)number;
    return asm_append_u32(section, value);
  }
  char *minus = strchr(item, '-');
  if (!minus)
    return false;
  *minus = '\0';
  char *symbol = trim_asm_space(item);
  char *rhs = trim_asm_space(minus + 1);
  int64_t addend = 0;
  if (strcmp(rhs, ".") != 0) {
    if (*rhs != '(')
      return false;
    ++rhs;
    rhs = trim_asm_space(rhs);
    if (*rhs++ != '.')
      return false;
    rhs = trim_asm_space(rhs);
    if (*rhs++ != '+')
      return false;
    rhs = trim_asm_space(rhs);
    char *addend_end = NULL;
    unsigned long parsed = strtoul(rhs, &addend_end, 0);
    if (!addend_end || *trim_asm_space(addend_end) != ')' ||
        *trim_asm_space(trim_asm_space(addend_end) + 1) || parsed > INT32_MAX)
      return false;
    addend = -(int64_t)parsed;
  }
  size_t symbol_len = strlen(symbol);
  bool local = symbol_len > 1 && symbol[symbol_len - 1] == 'b';
  for (size_t i = 0; local && i + 1 < symbol_len; ++i)
    local = isdigit((unsigned char)symbol[i]) != 0;
  if (local) {
    symbol[symbol_len - 1] = '\0';
    char *label_end = NULL;
    unsigned long label = strtoul(symbol, &label_end, 10);
    if (!label_end || *label_end)
      return false;
    size_t target_index = SIZE_MAX;
    size_t newest_label = 0;
    for (size_t i = 0; i < section_count; ++i) {
      if (sections[i].have_local_label && sections[i].local_label == label &&
          sections[i].local_label_order >= newest_label) {
        target_index = i;
        newest_label = sections[i].local_label_order;
      }
    }
    if (target_index == SIZE_MAX)
      return false;
    c_file_asm_section_t *target_section = &sections[target_index];
    if (target_section == section) {
      int64_t displacement = (int64_t)target_section->local_label_offset -
                             (int64_t)section->length + addend;
      uint32_t value = (uint32_t)displacement;
      return asm_append_u32(section, value);
    }
    if (target_section->symbol_count == 0) {
      char local_section_symbol[96];
      int written =
          snprintf(local_section_symbol, sizeof(local_section_symbol),
                   "__nyc_file_asm_%zu_section_%zu", asm_token, target_index);
      if (written < 0 || (size_t)written >= sizeof(local_section_symbol) ||
          !asm_section_add_symbol(target_section, local_section_symbol, 0))
        return false;
    }
    const char *target_symbol = target_section->symbols[0].name;
    if (!asm_add_pc32_reloc(section, target_symbol, strlen(target_symbol),
                            (int64_t)target_section->local_label_offset +
                                addend))
      return false;
    c_global_reloc_t *reloc = &section->relocs[section->reloc_count - 1];
    reloc->owned_target = strdup(target_symbol);
    if (!reloc->owned_target)
      return false;
    reloc->target.start = reloc->owned_target;
    return true;
  }
  if (!*symbol || symbol < assembly_work ||
      symbol + symbol_len > assembly_work + assembly_len)
    return false;
  for (const char *at = symbol; *at; ++at)
    if (!(isalnum((unsigned char)*at) || *at == '_' || *at == '.' ||
          *at == '$'))
      return false;
  const char *target = assembly + (symbol - assembly_work);
  return asm_add_pc32_reloc(section, target, symbol_len, addend);
}

static bool asm_emit_long_list(c_file_asm_section_t *section,
                               c_file_asm_section_t *sections,
                               size_t section_count, size_t asm_token,
                               char *values, char *assembly,
                               char *assembly_work, size_t assembly_len) {
  char *item = NULL;
  char *at = values;
  while (asm_next_list_item(&at, &item)) {
    if (!asm_emit_long_value(section, sections, section_count, asm_token, item,
                             assembly, assembly_work, assembly_len))
      return false;
  }
  return true;
}

static bool asm_emit_byte_list(c_file_asm_section_t *section, char *values) {
  char *item = NULL;
  char *at = values;
  while (asm_next_list_item(&at, &item)) {
    char *trimmed = trim_asm_space(item);
    char *endptr = NULL;
    unsigned long value = strtoul(trimmed, &endptr, 0);
    if (endptr == trimmed || *trim_asm_space(endptr) || value > UINT8_MAX)
      return false;
    unsigned char byte = (unsigned char)value;
    if (!asm_section_append(section, &byte, sizeof(byte)))
      return false;
  }
  return true;
}

static bool asm_parse_short_constant(char *text, int64_t *value) {
  text = trim_asm_space(text);
  size_t length = strlen(text);
  if (!length)
    return false;
  if (text[0] == '(' && text[length - 1] == ')') {
    text[length - 1] = '\0';
    return asm_parse_short_constant(text + 1, value);
  }
  if (text[0] == '~') {
    int64_t operand = 0;
    if (!asm_parse_short_constant(text + 1, &operand))
      return false;
    *value = ~operand;
    return true;
  }
  unsigned depth = 0;
  for (size_t i = 0; i + 1 < length; ++i) {
    if (text[i] == '(') {
      ++depth;
      continue;
    }
    if (text[i] == ')') {
      if (depth)
        --depth;
      continue;
    }
    if (depth || text[i] != '<' || text[i + 1] != '<')
      continue;
    text[i] = '\0';
    int64_t left = 0, right = 0;
    if (!asm_parse_short_constant(text, &left) ||
        !asm_parse_short_constant(text + i + 2, &right) || right < 0 ||
        right >= 63)
      return false;
    return !__builtin_mul_overflow(left, (int64_t)(UINT64_C(1) << right),
                                   value);
  }
  errno = 0;
  char *end = NULL;
  long long parsed = strtoll(text, &end, 0);
  if (end == text || errno == ERANGE)
    return false;
  while (*end == 'u' || *end == 'U' || *end == 'l' || *end == 'L')
    ++end;
  if (*trim_asm_space(end))
    return false;
  *value = (int64_t)parsed;
  return true;
}

static bool asm_parse_rsp_displacement(char *text, int32_t *displacement) {
  text = trim_asm_space(text);
  char *paren = strchr(text, '(');
  if (!paren || strcmp(trim_asm_space(paren), "(%rsp)") != 0)
    return false;
  *paren = '\0';
  char *expression = trim_asm_space(text);
  int64_t value = 0;
  char *multiply = strchr(expression, '*');
  if (multiply) {
    *multiply = '\0';
    int64_t left = 0, right = 0;
    if (!asm_parse_short_constant(expression, &left) ||
        !asm_parse_short_constant(multiply + 1, &right) ||
        __builtin_mul_overflow(left, right, &value))
      return false;
  } else if (*expression && !asm_parse_short_constant(expression, &value)) {
    return false;
  }
  if (value < INT32_MIN || value > INT32_MAX)
    return false;
  *displacement = (int32_t)value;
  return true;
}

static bool asm_emit_short_list(c_file_asm_section_t *section, char *values) {
  char *item = NULL;
  char *at = values;
  while (asm_next_list_item(&at, &item)) {
    item = trim_asm_space(item);
    int64_t value = 0;
    if (!asm_parse_short_constant(item, &value) || value < INT16_MIN ||
        value > UINT16_MAX)
      return false;
    uint16_t encoded = (uint16_t)value;
    unsigned char bytes[2] = {(unsigned char)encoded,
                              (unsigned char)(encoded >> 8)};
    if (!asm_section_append(section, bytes, sizeof(bytes)))
      return false;
  }
  return true;
}

static unsigned asm_x64_gpr_index(const char *name, bool wide64);

static bool asm_emit_movl_imm32(c_file_asm_section_t *section, char *text) {
  char *operand = trim_asm_space(text + 4);
  if (*operand++ != '$')
    return false;
  char *end = NULL;
  long long immediate = strtoll(operand, &end, 0);
  if (end == operand || immediate < INT32_MIN || immediate > UINT32_MAX)
    return false;
  char *destination = trim_asm_space(end);
  if (*destination++ != ',')
    return false;
  destination = trim_asm_space(destination);
  size_t length = strlen(destination);
  if (length < 4 || destination[0] != '(' || destination[length - 1] != ')')
    return false;
  destination[length - 1] = '\0';
  char *base = trim_asm_space(destination + 1);
  unsigned reg = asm_x64_gpr_index(base, true);
  if (reg == 16)
    return false;
  unsigned char encoded[8];
  size_t used = 0;
  if (reg >= 8)
    encoded[used++] = 0x41; /* REX.B */
  encoded[used++] = 0xc7;
  unsigned low = reg & 7u;
  if (low == 4) {
    encoded[used++] = 0x04; /* SIB follows */
    encoded[used++] = 0x24; /* no index, base rsp/r12 */
  } else if (low == 5) {
    encoded[used++] = 0x45; /* mod=01, /0, rbp/r13 */
    encoded[used++] = 0x00;
  } else {
    encoded[used++] = (unsigned char)(low & 7u); /* mod=00, /0 */
  }
  uint32_t value = (uint32_t)immediate;
  encoded[used++] = (unsigned char)value;
  encoded[used++] = (unsigned char)(value >> 8);
  encoded[used++] = (unsigned char)(value >> 16);
  encoded[used++] = (unsigned char)(value >> 24);
  return asm_section_append(section, encoded, used);
}

static bool asm_parse_pushq_constant(char *text, int64_t *result) {
  text = trim_asm_space(text);
  size_t length = strlen(text);
  if (length >= 3 && text[0] == '(' && text[length - 1] == ')') {
    text[length - 1] = '\0';
    text = trim_asm_space(text + 1);
  }
  errno = 0;
  char *end = NULL;
  long long left = strtoll(text, &end, 0);
  if (end == text || errno == ERANGE)
    return false;
  end = trim_asm_space(end);
  if (!*end) {
    *result = left;
    return true;
  }
  char operation = *end++;
  end = trim_asm_space(end);
  errno = 0;
  char *right_end = NULL;
  long long right = strtoll(end, &right_end, 0);
  if (right_end == end || errno == ERANGE || *trim_asm_space(right_end))
    return false;
  switch (operation) {
  case '+':
    return !__builtin_add_overflow((int64_t)left, (int64_t)right, result);
  case '-':
    return !__builtin_sub_overflow((int64_t)left, (int64_t)right, result);
  case '*':
    return !__builtin_mul_overflow((int64_t)left, (int64_t)right, result);
  default:
    return false;
  }
}

static bool asm_emit_pushq_imm32(c_file_asm_section_t *section, char *text,
                                 const char *assembly,
                                 const char *assembly_work,
                                 size_t assembly_len) {
  char *operand = trim_asm_space(text + 5);
  if (*operand++ != '$')
    return false;
  operand = trim_asm_space(operand);
  int64_t value = 0;
  if (asm_parse_pushq_constant(operand, &value) && value >= INT32_MIN &&
      value <= INT32_MAX) {
    if (value >= INT8_MIN && value <= INT8_MAX) {
      unsigned char instruction[] = {0x6a, (unsigned char)value};
      return asm_section_append(section, instruction, sizeof(instruction));
    }
    const unsigned char opcode = 0x68;
    int32_t immediate = (int32_t)value;
    return asm_section_append(section, &opcode, 1) &&
           asm_section_append(section, (const unsigned char *)&immediate,
                              sizeof(immediate));
  }
  size_t symbol_len = strlen(operand);
  if (!symbol_len || operand < assembly_work ||
      operand + symbol_len > assembly_work + assembly_len)
    return false;
  for (size_t i = 0; i < symbol_len; ++i)
    if (!(isalnum((unsigned char)operand[i]) || operand[i] == '_' ||
          operand[i] == '.' || operand[i] == '$'))
      return false;
  const unsigned char opcode = 0x68;
  return asm_section_append(section, &opcode, 1) &&
         asm_add_reloc(section, assembly + (operand - assembly_work),
                       symbol_len, 0, sizeof(uint32_t), NYC_DATA_RELOC_ABS32S);
}

static bool asm_emit_quad_list(c_file_asm_section_t *section, char *values,
                               char *assembly, char *assembly_work,
                               size_t assembly_len) {
  char *item = NULL;
  char *at = values;
  while (asm_next_list_item(&at, &item)) {
    item = trim_asm_space(item);
    char *endptr = NULL;
    errno = 0;
    unsigned long long number = strtoull(item, &endptr, 0);
    if (endptr != item && !*trim_asm_space(endptr) && errno != ERANGE) {
      uint64_t value = (uint64_t)number;
      if (!asm_section_append(section, &value, sizeof(value)))
        return false;
    } else {
      size_t symbol_len = strlen(item);
      if (!symbol_len || item < assembly_work ||
          item + symbol_len > assembly_work + assembly_len)
        return false;
      for (size_t i = 0; i < symbol_len; ++i)
        if (!(isalnum((unsigned char)item[i]) || item[i] == '_' ||
              item[i] == '.' || item[i] == '$'))
          return false;
      const char *target = assembly + (item - assembly_work);
      if (!asm_add_reloc(section, target, symbol_len, 0, sizeof(uint64_t),
                         NYC_DATA_RELOC_ABS64))
        return false;
    }
  }
  return true;
}

static bool asm_emit_string(c_file_asm_section_t *section, char *values,
                            bool terminate) {
  char *at = trim_asm_space(values);
  bool saw_string = false;
  while (*at) {
    if (*at == ',') {
      ++at;
      at = trim_asm_space(at);
    }
    if (*at != '"')
      return false;
    char *end = at + 1;
    while (*end) {
      if (*end == '\\' && end[1]) {
        end += 2;
        continue;
      }
      if (*end == '"')
        break;
      ++end;
    }
    if (*end != '"')
      return false;
    ny_ctok_t token = {
        .kind = NY_CTOK_STRING, .start = at, .len = (size_t)(end + 1 - at)};
    unsigned char *decoded = NULL;
    size_t decoded_len = 0;
    if (!decode_string_token(token, &decoded, &decoded_len) ||
        (decoded_len &&
         !asm_section_append(section, decoded, decoded_len - 1))) {
      free(decoded);
      return false;
    }
    free(decoded);
    at = trim_asm_space(end + 1);
    saw_string = true;
  }
  if (!saw_string)
    return false;
  if (terminate) {
    const unsigned char zero = 0;
    return asm_section_append(section, &zero, sizeof(zero));
  }
  return true;
}

static bool asm_label_referenced(const char *text, const char *after_label,
                                 const char *label) {
  size_t length = strlen(label);
  for (const char *at = after_label; (at = strstr(at, label)) != NULL; ++at) {
    unsigned char before = at == text ? '\0' : (unsigned char)at[-1];
    unsigned char after = (unsigned char)at[length];
    bool left_boundary =
        !isalnum(before) && before != '_' && before != '.' && before != '$';
    bool right_boundary =
        !isalnum(after) && after != '_' && after != '.' && after != '$';
    if (left_boundary && right_boundary && after != ':')
      return true;
  }
  return false;
}

static char *find_asm_label_colon(char *text) {
  bool in_string = false;
  bool escaped = false;
  for (char *at = text; *at; ++at) {
    if (in_string) {
      if (escaped)
        escaped = false;
      else if (*at == '\\')
        escaped = true;
      else if (*at == '"')
        in_string = false;
    } else if (*at == '"') {
      in_string = true;
    } else if (*at == ':') {
      return at;
    }
  }
  return NULL;
}

static unsigned asm_x64_gpr_index(const char *name, bool wide64) {
  static const char *const regs32[] = {
      "%eax", "%ecx", "%edx",  "%ebx",  "%esp",  "%ebp",  "%esi",  "%edi",
      "%r8d", "%r9d", "%r10d", "%r11d", "%r12d", "%r13d", "%r14d", "%r15d"};
  static const char *const regs64[] = {
      "%rax", "%rcx", "%rdx", "%rbx", "%rsp", "%rbp", "%rsi", "%rdi",
      "%r8",  "%r9",  "%r10", "%r11", "%r12", "%r13", "%r14", "%r15"};
  const char *const *registers = wide64 ? regs64 : regs32;
  for (unsigned i = 0; i < 16; ++i)
    if (strcmp(name, registers[i]) == 0)
      return i;
  return 16;
}

static bool asm_emit_stack_register(c_file_asm_section_t *section, char *text,
                                    bool pop) {
  size_t operand_offset =
      pop ? (text[3] == 'q' ? 5 : 4) : (text[4] == 'q' ? 6 : 5);
  char *reg_name = trim_asm_space(text + operand_offset);
  unsigned reg = asm_x64_gpr_index(reg_name, true);
  unsigned char opcode = (unsigned char)((pop ? 0x58u : 0x50u) + (reg & 7u));
  return reg < 16 && (reg < 8 || asm_section_append(section, "\x41", 1)) &&
         asm_section_append(section, &opcode, 1);
}

static bool asm_emit_rsp_adjust(c_file_asm_section_t *section, char *text,
                                bool subtract) {
  char *operand = trim_asm_space(text + 6);
  char *comma = strchr(operand, ',');
  int64_t immediate = -1;
  if (comma)
    *comma = '\0';
  bool valid = comma && strcmp(trim_asm_space(comma + 1), "%rsp") == 0 &&
               asm_parse_short_constant(operand, &immediate) &&
               immediate >= 0 && immediate <= INT8_MAX;
  if (!valid)
    return false;
  unsigned char opcode[] = {0x48, 0x83, subtract ? 0xec : 0xc4,
                            (unsigned char)immediate};
  return asm_section_append(section, opcode, sizeof(opcode));
}

static bool asm_emit_pc32_branch(c_file_asm_section_t *section, char *text,
                                 const char *assembly, char *assembly_work,
                                 size_t assembly_len, bool is_call) {
  size_t operand_offset = is_call ? 5 : 4;
  char *symbol = trim_asm_space(text + operand_offset);
  size_t symbol_len = strlen(symbol);
  bool valid = symbol_len && symbol >= assembly_work &&
               symbol + symbol_len <= assembly_work + assembly_len - 1;
  for (size_t i = 0; valid && i < symbol_len; ++i)
    valid = isalnum((unsigned char)symbol[i]) || symbol[i] == '_' ||
            symbol[i] == '.' || symbol[i] == '$';
  unsigned char opcode = is_call ? 0xe8 : 0xe9;
  return valid && asm_section_append(section, &opcode, 1) &&
         asm_add_pc32_reloc(section, assembly + (symbol - assembly_work),
                            symbol_len, -4);
}

/*
 * GCC asm labels on file-scope declarators: `asm (` string `)` after the
 * name. A register binding returns its raw string token to the caller.
 */
/*
 * Copy a raw (quoted) string-literal token, quotes stripped, into a
 * fixed-length buffer. Section and register names contain no escapes.
 */
static bool copy_quoted_token(c_parser_t *p, ny_ctok_t token, char *out,
                              size_t out_len) {
  if (token.kind != NY_CTOK_STRING || token.len < 2)
    return false;
  size_t trimmed = token.len - 2;
  if (trimmed + 1 >= out_len || !trimmed) {
    fail(p, "empty or oversized section/register name on a C declaration");
    return false;
  }
  if (memchr(token.start + 1, '\\', trimmed)) {
    fail(p, "section/register names must be plain string literals");
    return false;
  }
  memcpy(out, token.start + 1, trimmed);
  out[trimmed] = '\0';
  return true;
}

static bool try_parse_asm_label(c_parser_t *p, ny_ctok_t *register_name) {
  if (!tok_is(p, p->pos, "asm") && !tok_is(p, p->pos, "__asm") &&
      !tok_is(p, p->pos, "__asm__"))
    return true;
  size_t at = p->pos + 1;
  while (tok_is(p, at, "volatile") || tok_is(p, at, "goto"))
    ++at;
  if (!tok_is(p, at, "("))
    return true;
  ny_ctok_t label = {0};
  if (p->tokens[at + 1].kind == NY_CTOK_STRING && tok_is(p, at + 2, ")")) {
    label = p->tokens[at + 1];
    p->pos = at + 3;
    *register_name = label;
    return true;
  }
  /*
   * Balanced parenthetical without a leading string literal: skip it and
   * keep the declaration parseable without recording a register binding.
   */
  size_t end = skip_balanced(p, at, "(", ")");
  if (end <= at || end > p->token_count) {
    fail(p, "unterminated asm label on a C declaration");
    return false;
  }
  p->pos = end;
  return true;
}

/*
 * Only volatile reads of an x86-64 global register bound to RSP are lowered.
 * Other bindings and writes still need reserved-register code generation.
 */
static bool global_register_use_fails(c_parser_t *p, const c_global_t *global) {
  if (!global || !global->has_asm_register)
    return false;
  char message[256];
  int register_len = (int)global->asm_register.len - 2;
  if (register_len < 0)
    register_len = 0;
  if (register_len > 130)
    register_len = 130;
  int name_len = (int)global->name.len;
  if (name_len > 100)
    name_len = 100;
  int written = snprintf(message, sizeof(message),
                         "global register variable `%.*s` bound to "
                         "`%.*s` use is unsupported by NYC",
                         name_len, global->name.start, register_len,
                         global->asm_register.start + 1);
  if (written < 0 || (size_t)written >= sizeof(message))
    snprintf(message, sizeof(message),
             "global register variable uses require reserved-register code "
             "generation, which is not supported");
  fail(p, message);
  return true;
}

static bool global_register_is_x86_rsp(const c_global_t *global) {
  return global && global->has_asm_register &&
         global->asm_register.len == sizeof("\"rsp\"") - 1 &&
         memcmp(global->asm_register.start, "\"rsp\"", sizeof("\"rsp\"") - 1) ==
             0;
}

static bool global_register_is_x86_stack_pointer(nyc_abi_t abi,
                                                 const c_global_t *global) {
  if (!global || !global->has_asm_register)
    return false;
  const char *name = abi == NYC_ABI_I386 ? "\"esp\"" : "\"rsp\"";
  size_t len = strlen(name);
  return global->asm_register.len == len &&
         memcmp(global->asm_register.start, name, len) == 0;
}
#endif
