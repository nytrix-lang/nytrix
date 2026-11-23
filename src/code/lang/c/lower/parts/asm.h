/* Inline assembly statement lowering, included by parse.c. */
static bool inline_asm_string_sequence_is(const c_parser_t *p, const char *text,
                                          size_t *end) {
  size_t len = strlen(text);
  size_t matched = 0;
  size_t at = p->pos;
  while (at < p->token_count && p->tokens[at].kind == NY_CTOK_STRING) {
    unsigned char *part = NULL;
    size_t part_len = 0;
    if (!decode_string_token(p->tokens[at], &part, &part_len)) {
      free(part);
      return false;
    }
    size_t content_len = part_len ? part_len - 1 : 0;
    bool matches = content_len <= len - matched &&
                   memcmp(part, text + matched, content_len) == 0;
    free(part);
    if (!matches)
      return false;
    matched += content_len;
    ++at;
  }
  if (matched != len)
    return false;
  *end = at;
  return true;
}

static bool inline_asm_template_is(const unsigned char *assembly, size_t len,
                                   const char *expected) {
  size_t actual_at = 0;
  size_t expected_at = 0;
  for (;;) {
    while (actual_at < len && isspace(assembly[actual_at]))
      ++actual_at;
    while (expected[expected_at] &&
           isspace((unsigned char)expected[expected_at]))
      ++expected_at;
    if (actual_at == len || !expected[expected_at])
      return actual_at == len && expected[expected_at] == '\0';
    if (assembly[actual_at++] != (unsigned char)expected[expected_at++])
      return false;
  }
}

static bool inline_asm_statement_has_token(const c_parser_t *p, size_t open,
                                           const char *token) {
  if (!p || !token)
    return false;
  size_t depth = 0;
  for (size_t i = open; i < p->token_count; ++i) {
    if (tok_is(p, i, "("))
      ++depth;
    else if (tok_is(p, i, ")")) {
      if (depth == 0 || --depth == 0)
        break;
    }
    if (tok_is(p, i, token))
      return true;
  }
  return false;
}

static bool seek_inline_asm_constraint(c_parser_t *p, const char *constraint) {
  size_t limit = p->pos + 16;
  if (limit > p->token_count)
    limit = p->token_count;
  for (size_t at = p->pos; at < limit; ++at) {
    c_parser_t probe = *p;
    size_t end;
    probe.pos = at;
    if (inline_asm_string_sequence_is(&probe, constraint, &end) &&
        tok_is(&probe, end, "(")) {
      p->pos = at;
      return true;
    }
  }
  return false;
}

static int lower_asm_memory_output_address(c_parser_t *p,
                                           const c_lvalue_t *lvalue) {
  if (!lvalue || lvalue->bitfield || !discard_lvalue_read(p, lvalue))
    return -1;
  if (lvalue->is_local && lvalue->type != C_AGG) {
    c_local_t *local = find_local_slot(p, lvalue->local_slot);
    if (!local || local->is_const || local->array_rank ||
        local->fixed_register.len)
      return -1;
    local->has_integer_constant = false;
    local->address_taken = true;
    if (lvalue->address < 0)
      return emit_value(p, (nyr_inst_t){.op = NYIR_ADDR_LOCAL,
                                        .dst = -1,
                                        .imm = (int64_t)local->slot,
                                        .scalar_class = NYIR_SCALAR_I64_RAW});
  }
  return lvalue->address;
}

static bool inline_asm_clobber_list_matches(const c_parser_t *p, size_t *end,
                                            bool memory_output) {
  static const char *const clobbers[] = {
      "memory", "cc",    "flags", "rax",   "rbx",  "rcx",  "rdx",  "rsi",
      "rdi",    "rbp",   "rsp",   "di",    "si",   "r8",   "r9",   "r10",
      "r11",    "r12",   "r13",   "r14",   "r15",  "eax",  "ebx",  "ecx",
      "edx",    "esi",   "edi",   "ebp",   "esp",  "xmm0", "xmm1", "xmm2",
      "xmm3",   "xmm4",  "xmm5",  "xmm6",  "xmm7", "xmm8", "xmm9", "xmm10",
      "xmm11",  "xmm12", "xmm13", "xmm14", "xmm15"};
  size_t at = p->pos;
  bool found = false;
  while (at < p->token_count) {
    char name[16] = {0};
    size_t length = 0;
    while (at < p->token_count && p->tokens[at].kind == NY_CTOK_STRING) {
      unsigned char *part = NULL;
      size_t part_len = 0;
      if (!decode_string_token(p->tokens[at], &part, &part_len))
        return false;
      size_t content = part_len - 1;
      if (content >= sizeof(name) - length || memchr(part, 0, content)) {
        free(part);
        return false;
      }
      memcpy(name + length, part, content);
      length += content;
      free(part);
      ++at;
    }
    if (!length)
      return false;
    bool recognized = false;
    for (size_t i = 0; i < sizeof(clobbers) / sizeof(clobbers[0]); ++i) {
      if (strcmp(name, clobbers[i]) == 0) {
        recognized = true;
        break;
      }
    }
    if (!recognized) {
      if (!getenv("NYC_KERNEL_COMPAT"))
        return false;
    }
    if (memory_output) {
      /*
       * The generic memory emitter reserves RAX for the address and does
       * not save arbitrary callee-saved registers around an asm statement.
       */
      bool fixed_or_saved =
          strcmp(name, "rax") == 0 || strcmp(name, "eax") == 0 ||
          strcmp(name, "rbx") == 0 || strcmp(name, "ebx") == 0 ||
          strcmp(name, "rbp") == 0 || strcmp(name, "ebp") == 0 ||
          strcmp(name, "rsp") == 0 || strcmp(name, "esp") == 0 ||
          (name[0] == 'r' && name[1] == '1' && name[2] >= '2' &&
           name[2] <= '5' && !name[3]);
      bool additional_saved =
          ((p->abi == NYC_ABI_WIN64 || p->abi == NYC_ABI_I386) &&
           (strcmp(name, "rsi") == 0 || strcmp(name, "esi") == 0 ||
            strcmp(name, "si") == 0 || strcmp(name, "rdi") == 0 ||
            strcmp(name, "edi") == 0 || strcmp(name, "di") == 0)) ||
          (p->abi == NYC_ABI_WIN64 && strncmp(name, "xmm", 3) == 0 &&
           (name[3] >= '6' || name[4]));
      if (fixed_or_saved || additional_saved)
        return false;
    }
    found = true;
    if (!tok_is(p, at, ","))
      break;
    ++at;
    if (tok_is(p, at, ")"))
      break;
  }
  if (!found)
    return false;
  *end = at;
  return true;
}

static bool inline_asm_clobber_list_is(const c_parser_t *p, size_t *end) {
  return inline_asm_clobber_list_matches(p, end, false);
}

static bool parse_inline_asm_operand_name(c_parser_t *p) {
  if (!accept(p, "["))
    return true;
  ny_ctok_t name;
  return parse_name(p, &name) && expect(p, "]");
}

static bool parse_named_inline_asm_operand(c_parser_t *p, ny_ctok_t *name) {
  *name = (ny_ctok_t){0};
  if (!accept(p, "["))
    return true;
  return parse_name(p, name) && expect(p, "]");
}

typedef struct {
  ny_ctok_t name;
  const char *replacement;
} inline_asm_named_value_t;

static const char *inline_asm_integer_register_name(c_type_t type) {
  switch (type) {
  case C_U8:
  case C_CHAR:
    return "%dl";
  case C_I16:
  case C_U16:
    return "%dx";
  case C_I32:
  case C_U32:
    return "%edx";
  default:
    return "%rdx";
  }
}

static bool inline_asm_name_matches(ny_ctok_t name, const unsigned char *text,
                                    size_t length) {
  return name.kind == NY_CTOK_IDENT && name.len == length &&
         memcmp(name.start, text, length) == 0;
}

static bool inline_asm_has_named_operand(const unsigned char *assembly,
                                         size_t length) {
  for (size_t i = 0; i + 1 < length; ++i) {
    if (assembly[i] == '%' &&
        (assembly[i + 1] == '[' ||
         (assembly[i + 1] == 'c' && i + 2 < length && assembly[i + 2] == '[')))
      return true;
  }
  return false;
}

static bool substitute_inline_asm_named_operands(
    unsigned char **assembly, size_t *assembly_len, size_t *assembly_cap,
    ny_ctok_t output_name, const char *output_replacement,
    const inline_asm_named_value_t *inputs, size_t input_count) {
  unsigned char *expanded = NULL;
  size_t expanded_len = 0;
  size_t expanded_cap = 0;
  bool replaced = false;
  for (size_t i = 0; i < *assembly_len; ++i) {
    if ((*assembly)[i] == '%' && i + 1 < *assembly_len &&
        (*assembly)[i + 1] == '%') {
      if (!append_asm_bytes(&expanded, &expanded_len, &expanded_cap, "%", 1)) {
        free(expanded);
        return false;
      }
      ++i;
      continue;
    }
    if ((*assembly)[i] == '%' && i + 2 < *assembly_len) {
      bool immediate = (*assembly)[i + 1] == 'c' && (*assembly)[i + 2] == '[';
      bool address = (*assembly)[i + 1] == 'a' && (*assembly)[i + 2] == '[';
      size_t name_start = i + (immediate || address ? 3 : 2);
      if ((immediate || address || (*assembly)[i + 1] == '[') &&
          name_start < *assembly_len) {
        size_t end = name_start;
        while (end < *assembly_len &&
               (isalnum((*assembly)[end]) || (*assembly)[end] == '_'))
          ++end;
        if (end > name_start && end < *assembly_len &&
            (*assembly)[end] == ']') {
          const char *replacement = NULL;
          if (!immediate && !address &&
              inline_asm_name_matches(output_name, *assembly + name_start,
                                      end - name_start)) {
            replacement = output_replacement;
          } else {
            for (size_t input = 0; input < input_count; ++input) {
              if (inline_asm_name_matches(inputs[input].name,
                                          *assembly + name_start,
                                          end - name_start)) {
                replacement = inputs[input].replacement;
                break;
              }
            }
          }
          if (!replacement ||
              !append_asm_bytes(&expanded, &expanded_len, &expanded_cap,
                                replacement, strlen(replacement))) {
            free(expanded);
            return false;
          }
          i = end;
          replaced = true;
          continue;
        }
      }
    }
    if (!append_asm_bytes(&expanded, &expanded_len, &expanded_cap,
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

static bool substitute_inline_asm_alt_io_operands(unsigned char **assembly,
                                                  size_t *assembly_len,
                                                  size_t *assembly_cap) {
  unsigned char *expanded = NULL;
  size_t expanded_len = 0, expanded_cap = 0;
  bool have_register = false, have_memory = false;
  for (size_t i = 0; i < *assembly_len; ++i) {
    const char *replacement = NULL;
    if ((*assembly)[i] == '%' && i + 1 < *assembly_len &&
        ((*assembly)[i + 1] == '0' || (*assembly)[i + 1] == '1')) {
      if ((*assembly)[i + 1] == '0') {
        replacement = "%edx";
        have_register = true;
      } else {
        replacement = "(%rax)";
        have_memory = true;
      }
      ++i;
    }
    if (replacement) {
      if (!append_asm_bytes(&expanded, &expanded_len, &expanded_cap,
                            (const unsigned char *)replacement,
                            strlen(replacement)))
        goto fail;
    } else if (!append_asm_bytes(&expanded, &expanded_len, &expanded_cap,
                                 *assembly + i, 1)) {
      goto fail;
    }
  }
  if (!have_register || !have_memory)
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

/*
 * `pv_ops.irq.safe_halt` reaches the IR as a load from `symbol + offset`.
 * Recover that address so the paravirt call can name it in its own assembly
 * (objtool recognises a paravirt call by the `pv_ops` relocation on the call).
 */
static bool paravirt_member_address(const c_parser_t *p, int value,
                                    char *symbol, size_t symbol_size,
                                    int64_t *offset) {
  const nyr_func_t *f = p->out;
  const nyr_inst_t *load = NULL, *base = NULL;
  int64_t total = 0;
  for (size_t i = f->len; i-- > 0;)
    if (f->data[i].dst == value) {
      load = &f->data[i];
      break;
    }
  if (!load || load->op != NYIR_LOAD_I64 || load->a < 0)
    return false;
  total = load->imm;
  const nyr_inst_t *address = NULL;
  for (size_t i = f->len; i-- > 0;)
    if (f->data[i].dst == load->a) {
      address = &f->data[i];
      break;
    }
  if (!address)
    return false;
  if (address->op == NYIR_ADD_I64 && address->a >= 0 && address->b >= 0) {
    const nyr_inst_t *left = NULL, *right = NULL;
    for (size_t i = f->len; i-- > 0 && (!left || !right);) {
      if (f->data[i].dst == address->a)
        left = &f->data[i];
      if (f->data[i].dst == address->b)
        right = &f->data[i];
    }
    if (!left || !right || right->op != NYIR_CONST_I64)
      return false;
    total += right->imm;
    base = left;
  } else {
    base = address;
  }
  static const char extern_prefix[] = "__nyc_extern_";
  if (base->op != NYIR_ADDR_SYMBOL || !base->symbol ||
      strncmp(base->symbol, extern_prefix, sizeof(extern_prefix) - 1) != 0 ||
      total < INT32_MIN / 2 || total > INT32_MAX / 2)
    return false;
  const char *name = base->symbol + sizeof(extern_prefix) - 1;
  if (!*name || strlen(name) >= symbol_size)
    return false;
  strcpy(symbol, name);
  *offset = total;
  return true;
}

static bool defer_unsupported_asm(c_parser_t *p, size_t open,
                                  unsigned char *assembly) {
  if (p->err && p->err_len)
    p->err[0] = '\0';
  if (!p->function) {
    char message[160];
    snprintf(message, sizeof(message),
             "unsupported GNU inline-assembly operands for template: %.96s",
             assembly ? (const char *)assembly : "<empty>");
    fail(p, message);
    free(assembly);
    return false;
  }
  size_t after = skip_balanced(p, open, "(", ")");
  if (after <= open + 1 || after >= p->token_count || !tok_is(p, after, ";")) {
    fail(p, "malformed GNU inline-assembly statement");
    free(assembly);
    return false;
  }
  /* NYC kernel compat: lower simple MMIO/pointer asm reads/writes as volatile
   * loads/stores. */
  if (getenv("NYC_KERNEL_COMPAT") && assembly) {
    const char *t = (const char *)assembly;
    unsigned mem_flags = 0;
    const char *read_pat = NULL;
    if (strstr(t, "movb (") &&
        (strstr(t, "),%al") || strstr(t, "),%%al") || strstr(t, "), %al"))) {
      mem_flags = NYIR_INST_F_MEM_BYTE;
      read_pat = "b";
    } else if (strstr(t, "movw (") &&
               (strstr(t, "),%ax") || strstr(t, "),%%ax") ||
                strstr(t, "), %ax"))) {
      mem_flags = NYIR_INST_F_MEM_U16;
      read_pat = "w";
    } else if (strstr(t, "movl (") &&
               (strstr(t, "),%eax") || strstr(t, "),%%eax") ||
                strstr(t, "), %eax"))) {
      mem_flags = NYIR_INST_F_MEM_I32;
      read_pat = "l";
    } else if (strstr(t, "movq (") &&
               (strstr(t, "),%rax") || strstr(t, "),%%rax") ||
                strstr(t, "), %rax"))) {
      read_pat = "q";
    }
    if (read_pat) {
      c_parser_t saved = *p;
      char saved_err[1] = {0};
      char *old_err = p->err;
      size_t old_err_len = p->err_len;
      p->err = saved_err;
      p->err_len = sizeof(saved_err);
      /* find post-template ':' */
      size_t pos = open + 1;
      while (pos < after && p->tokens[pos].kind == NY_CTOK_STRING)
        ++pos;
      if (pos < after && tok_is(p, pos, ":")) {
        ++pos; /* outputs */
        /* parse output constraint + (lvalue) */
        while (pos < after && !tok_is(p, pos, "(") && !tok_is(p, pos, ":"))
          ++pos;
        if (pos < after && tok_is(p, pos, "(")) {
          p->pos = pos + 1;
          c_value_t outv = parse_expr(p, 1);
          const c_lvalue_t *outlv =
              outv.value >= 0 ? find_lvalue(p, outv.value) : NULL;
          if (outlv && expect(p, ")")) {
            /* skip to inputs ':' */
            while (p->pos < after && !tok_is(p, p->pos, ":"))
              ++p->pos;
            if (p->pos < after && tok_is(p, p->pos, ":")) {
              ++p->pos;
              while (p->pos < after && !tok_is(p, p->pos, "(") &&
                     !tok_is(p, p->pos, ":"))
                ++p->pos;
              if (p->pos < after && tok_is(p, p->pos, "(")) {
                p->pos++;
                c_value_t ptrv = parse_expr(p, 1);
                if (ptrv.value >= 0 && expect(p, ")")) {
                  int loaded = emit_value(
                      p, (nyr_inst_t){.op = NYIR_LOAD_I64,
                                      .dst = -1,
                                      .a = ptrv.value,
                                      .effects = NYIR_EFFECT_READ_MEMORY |
                                                 NYIR_EFFECT_MAY_TRAP |
                                                 NYIR_EFFECT_VOLATILE,
                                      .flags = mem_flags});
                  bool ok = loaded >= 0;
                  if (ok && outlv->is_local)
                    ok = emit(p,
                              (nyr_inst_t){.op = NYIR_STORE_LOCAL,
                                           .dst = -1,
                                           .a = (int)outlv->local_slot,
                                           .c = loaded,
                                           .effects = NYIR_EFFECT_WRITE_MEMORY,
                                           .scalar_class = NYIR_SCALAR_I64_RAW},
                              NULL);
                  else if (ok && outlv->address >= 0) {
                    unsigned sf = 0;
                    if (c_type_size(p->abi, outlv->type) == 1)
                      sf = NYIR_INST_F_MEM_BYTE;
                    else if (c_type_size(p->abi, outlv->type) == 2)
                      sf = NYIR_INST_F_MEM_U16;
                    else if (c_type_size(p->abi, outlv->type) == 4)
                      sf = NYIR_INST_F_MEM_I32;
                    ok = emit(p,
                              (nyr_inst_t){.op = NYIR_STORE_I64,
                                           .dst = -1,
                                           .a = outlv->address,
                                           .c = loaded,
                                           .effects = NYIR_EFFECT_WRITE_MEMORY |
                                                      NYIR_EFFECT_MAY_TRAP,
                                           .flags = sf,
                                           .scalar_class = NYIR_SCALAR_I64_RAW},
                              NULL);
                  } else
                    ok = false;
                  if (ok) {
                    p->err = old_err;
                    p->err_len = old_err_len;
                    p->pos = after + 1;
                    free(assembly);
                    return true;
                  }
                }
              }
            }
          }
        }
      }
      *p = saved;
      p->err = old_err;
      p->err_len = old_err_len;
    }
    /* MMIO/pointer writes: movb/w/l/q %al,(%1) style with two inputs (val, ptr)
     */
    unsigned wflags = 0;
    bool wpat = false;
    if ((strstr(t, "movb %al,") || strstr(t, "movb %%al,") ||
         strstr(t, "movb\t%al,")) &&
        strstr(t, "(%")) {
      wflags = NYIR_INST_F_MEM_BYTE;
      wpat = true;
    } else if ((strstr(t, "movw %ax,") || strstr(t, "movw %%ax,")) &&
               strstr(t, "(%")) {
      wflags = NYIR_INST_F_MEM_U16;
      wpat = true;
    } else if ((strstr(t, "movl %eax,") || strstr(t, "movl %%eax,")) &&
               strstr(t, "(%")) {
      wflags = NYIR_INST_F_MEM_I32;
      wpat = true;
    } else if ((strstr(t, "movq %rax,") || strstr(t, "movq %%rax,")) &&
               strstr(t, "(%")) {
      wpat = true;
    }
    if (wpat) {
      c_parser_t saved = *p;
      char saved_err[1] = {0};
      char *old_err = p->err;
      size_t old_err_len = p->err_len;
      p->err = saved_err;
      p->err_len = sizeof(saved_err);
      size_t pos = open + 1;
      while (pos < after && p->tokens[pos].kind == NY_CTOK_STRING)
        ++pos;
      /* skip outputs section (possibly empty) to inputs */
      if (pos < after && tok_is(p, pos, ":"))
        ++pos;
      while (pos < after && !tok_is(p, pos, ":")) {
        if (tok_is(p, pos, "(")) {
          int d = 1;
          ++pos;
          while (pos < after && d) {
            if (tok_is(p, pos, "("))
              d++;
            else if (tok_is(p, pos, ")"))
              d--;
            ++pos;
          }
        } else
          ++pos;
      }
      if (pos < after && tok_is(p, pos, ":")) {
        ++pos;
        /* parse first input (value) */
        while (pos < after && !tok_is(p, pos, "(") && !tok_is(p, pos, ":"))
          ++pos;
        if (pos < after && tok_is(p, pos, "(")) {
          p->pos = pos + 1;
          c_value_t valv = parse_expr(p, 1);
          if (valv.value >= 0 && expect(p, ")")) {
            if (accept(p, ",")) {
            }
            while (p->pos < after && !tok_is(p, p->pos, "(") &&
                   !tok_is(p, p->pos, ":") && !tok_is(p, p->pos, ")"))
              ++p->pos;
            if (p->pos < after && tok_is(p, p->pos, "(")) {
              p->pos++;
              c_value_t ptrv = parse_expr(p, 1);
              if (ptrv.value >= 0 && expect(p, ")")) {
                bool ok =
                    emit(p,
                         (nyr_inst_t){.op = NYIR_STORE_I64,
                                      .dst = -1,
                                      .a = ptrv.value,
                                      .c = valv.value,
                                      .effects = NYIR_EFFECT_WRITE_MEMORY |
                                                 NYIR_EFFECT_MAY_TRAP |
                                                 NYIR_EFFECT_VOLATILE,
                                      .flags = wflags,
                                      .scalar_class = NYIR_SCALAR_I64_RAW},
                         NULL);
                if (ok) {
                  p->err = old_err;
                  p->err_len = old_err_len;
                  p->pos = after + 1;
                  free(assembly);
                  return true;
                }
              }
            }
          }
        }
      }
      *p = saved;
      p->err = old_err;
      p->err_len = old_err_len;
    }
  }
  p->pos = after + 1;
  const char *const prefix = "__nyc_unsupported_inline_asm:";
  size_t template_len = assembly ? strlen((const char *)assembly) : 0;
  char source_asm[384] = {0};
  size_t source_asm_len = 0;
  for (size_t i = open; i < after && source_asm_len < sizeof(source_asm) - 2;
       ++i) {
    ny_ctok_t token = p->tokens[i];
    if (source_asm_len)
      source_asm[source_asm_len++] = ' ';
    size_t copy_len = token.len;
    if (copy_len > sizeof(source_asm) - source_asm_len - 2)
      copy_len = sizeof(source_asm) - source_asm_len - 2;
    if (copy_len)
      memcpy(source_asm + source_asm_len, token.start, copy_len);
    source_asm_len += copy_len;
  }
  source_asm[source_asm_len] = '\0';
  size_t prefix_len = sizeof("__nyc_unsupported_inline_asm:") - 1;
  static const char source_prefix[] = " | source: ";
  size_t detail_len =
      source_asm_len ? sizeof(source_prefix) - 1 + source_asm_len : 0;
  if (!p->out || detail_len > SIZE_MAX - template_len ||
      template_len + detail_len > SIZE_MAX - prefix_len - 1) {
    fail(p, "out of memory preserving unsupported inline-assembly details");
    free(assembly);
    return false;
  }
  char *unsupported = malloc(prefix_len + template_len + detail_len + 1);
  if (!unsupported) {
    fail(p, "out of memory preserving unsupported inline-assembly details");
    free(assembly);
    return false;
  }
  memcpy(unsupported, prefix, prefix_len);
  if (template_len)
    memcpy(unsupported + prefix_len, assembly, template_len);
  if (detail_len) {
    memcpy(unsupported + prefix_len + template_len, source_prefix,
           sizeof(source_prefix) - 1);
    memcpy(unsupported + prefix_len + template_len + sizeof(source_prefix) - 1,
           source_asm, source_asm_len);
  }
  unsupported[prefix_len + template_len + detail_len] = '\0';
  const char *owned = nyr_func_own_symbol_copy(p->out, unsupported);
  free(unsupported);
  free(assembly);
  if (!owned) {
    fail(p, "out of memory preserving unsupported inline-assembly details");
    return false;
  }
  return emit(
      p,
      (nyr_inst_t){.op = NYIR_INLINE_ASM, .dst = -1, .a = -1, .symbol = owned},
      NULL);
}

#define NYTRIX_C_PARSE_INLINE_ASM_HELPERS
#include "code/lang/c/parser/parts/asm.h"
#undef NYTRIX_C_PARSE_INLINE_ASM_HELPERS

static bool lower_inline_asm_statement(c_parser_t *p) {
  ++p->pos;
  bool asm_goto = false;
  for (;;) {
    if (accept(p, "volatile") || accept(p, "__volatile") ||
        accept(p, "__volatile__") || accept(p, "inline") ||
        accept(p, "__inline") || accept(p, "__inline__"))
      continue;
    break;
  }
  /*
   * Inline asm is conservatively treated as volatile in NYIR, regardless of
   * the source qualifier.
   */
  asm_goto = accept(p, "goto");
  size_t open = p->pos;
  if (!expect(p, "("))
    return false;

  unsigned char *assembly = NULL;
  size_t assembly_len = 0;
  size_t assembly_cap = 0;
  bool have_template = false;
  while (current(p)->kind == NY_CTOK_STRING) {
    unsigned char *part = NULL;
    size_t part_len = 0;
    if (!decode_string_token(*current(p), &part, &part_len) ||
        (part_len && memchr(part, '\0', part_len - 1)) ||
        !append_asm_bytes(&assembly, &assembly_len, &assembly_cap, part,
                          part_len ? part_len - 1 : 0)) {
      free(part);
      fail(p, "invalid GNU inline-assembly template");
      free(assembly);
      return false;
    }
    free(part);
    have_template = true;
    ++p->pos;
  }
  if (!have_template) {
    fail(p, "GNU inline assembly requires a string-literal template");
    free(assembly);
    return false;
  }
  if (p->function && (memmem(assembly, assembly_len, ".globl", 6) ||
                      memmem(assembly, assembly_len, ".global", 7)))
    p->function->noinline = true;
  static const struct {
    const char *assembly;
    const char *symbol;
    size_t input_size;
  } crc_templates[] = {
      {"crc32q %1, %q0", "crc32q", 8},
      {"crc32l %1, %0", "crc32l", 4},
      {"crc32w %1, %0", "crc32w", 2},
      {"crc32b %1, %0", "crc32b", 1},
  };
  const char *crc_symbol = NULL;
  size_t crc_input_size = 0;
  for (size_t i = 0; i < sizeof(crc_templates) / sizeof(crc_templates[0]);
       ++i) {
    if (inline_asm_template_ignoring_whitespace(assembly, assembly_len,
                                                crc_templates[i].assembly)) {
      crc_symbol = crc_templates[i].symbol;
      crc_input_size = crc_templates[i].input_size;
      break;
    }
  }
  if (!asm_goto && crc_symbol) {
    if (!expect(p, ":") || !parse_inline_asm_operand_name(p) ||
        !inline_asm_string_sequence_is(p, "+r", &p->pos) || !expect(p, "(")) {
      free(assembly);
      return false;
    }
    c_value_t output = parse_expr(p, 1);
    const c_lvalue_t *output_lvalue =
        output.value >= 0 ? find_lvalue(p, output.value) : NULL;
    c_local_t *output_local =
        output_lvalue && output_lvalue->is_local &&
                output_lvalue->local_slot >= 0
            ? find_local_slot(p, (size_t)output_lvalue->local_slot)
            : NULL;
    if (!output_local || (output.type != C_I32 && output.type != C_U32) ||
        !expect(p, ")") || !expect(p, ":") ||
        !parse_inline_asm_operand_name(p)) {
      free(assembly);
      return false;
    }
    size_t input_constraint_end = p->pos;
    if (!(inline_asm_string_sequence_is(p, "rm", &input_constraint_end) ||
          inline_asm_string_sequence_is(p, "r", &input_constraint_end))) {
      free(assembly);
      return false;
    }
    p->pos = input_constraint_end;
    if (!expect(p, "(")) {
      free(assembly);
      return false;
    }
    c_value_t input = parse_expr(p, 1);
    if (input.value < 0 || !c_type_is_integer(input.type) ||
        c_type_size(p->abi, input.type) != crc_input_size || !expect(p, ")") ||
        !expect(p, ")") || !expect(p, ";")) {
      free(assembly);
      return false;
    }
    int result = emit_value(p, (nyr_inst_t){.op = NYIR_INLINE_ASM,
                                            .dst = -1,
                                            .a = output.value,
                                            .b = input.value,
                                            .c = -1,
                                            .d = -1,
                                            .e = -1,
                                            .f = -1,
                                            .imm = -180,
                                            .symbol = crc_symbol});
    bool ok = result >= 0 && lower_local_store(p, output_local, result);
    free(assembly);
    return ok;
  }

  static const char endbr_constant_template[] = "mov $~0xfa1e0ff3, %[endbr]\n"
                                                "not %[endbr]\n";
  if (!asm_goto && inline_asm_template_ignoring_whitespace(
                       assembly, assembly_len, endbr_constant_template)) {
    if (!expect(p, ":") || !parse_inline_asm_operand_name(p) ||
        !inline_asm_string_sequence_is(p, "=&r", &p->pos) || !expect(p, "(")) {
      free(assembly);
      return false;
    }
    c_value_t output = parse_expr(p, 1);
    const c_lvalue_t *lvalue =
        output.value >= 0 ? find_lvalue(p, output.value) : NULL;
    c_local_t *local = lvalue && lvalue->is_local && lvalue->local_slot >= 0
                           ? find_local_slot(p, (size_t)lvalue->local_slot)
                           : NULL;
    if (output.value < 0 || !local ||
        (output.type != C_I32 && output.type != C_U32) || !expect(p, ")") ||
        !expect(p, ")") || !expect(p, ";")) {
      free(assembly);
      return false;
    }
    int constant = emit_i64_constant(p, (int64_t)UINT32_C(0xfa1e0ff3));
    bool ok = constant >= 0 && lower_local_store(p, local, constant);
    free(assembly);
    return ok;
  }

  bool usercopy_movnti_4x8 = INLINE_ASM_HAS("movq  24(%0), %%r11");
  bool usercopy_movnti_1x8 =
      INLINE_ASM_HAS("movq    (%0), %%r8") && !usercopy_movnti_4x8;
  bool usercopy_movnti_1x4 = INLINE_ASM_HAS("movnti  %%r8d,   (%1)");
  static const char ip_fast_csum_template_text[] = "movl (%1), %0\n"
                                                   "subl $4, %2\n"
                                                   "jbe 2f\n"
                                                   "addl 4(%1), %0\n"
                                                   "adcl 8(%1), %0\n"
                                                   "adcl 12(%1), %0\n"
                                                   "1: adcl 16(%1), %0\n"
                                                   "lea 4(%1), %1\n"
                                                   "decl %2\n"
                                                   "jne 1b\n"
                                                   "adcl $0, %0\n"
                                                   "movl %0, %2\n"
                                                   "shrl $16, %0\n"
                                                   "addw %w2, %w0\n"
                                                   "adcl $0, %0\n"
                                                   "notl %0\n"
                                                   "2:";
  bool ip_fast_csum_template = inline_asm_template_ignoring_whitespace(
      assembly, assembly_len, ip_fast_csum_template_text);
  const char *usercopy_movnti_symbol =
      usercopy_movnti_4x8   ? "usercopy_movnti_4x8"
      : usercopy_movnti_1x8 ? "usercopy_movnti_1x8"
      : usercopy_movnti_1x4 ? "usercopy_movnti_1x4"
                            : NULL;
  if (!asm_goto && usercopy_movnti_symbol) {
    if (!expect(p, ":") || !expect(p, ":") ||
        !inline_asm_string_sequence_is(p, "r", &p->pos) || !expect(p, "("))
      return defer_unsupported_asm(p, open, assembly);
    c_value_t source = parse_expr(p, 1);
    if (source.value < 0 ||
        (!c_type_is_pointer(source.type) && !c_type_is_integer(source.type)) ||
        !expect(p, ")") || !expect(p, ",") ||
        !inline_asm_string_sequence_is(p, "r", &p->pos) || !expect(p, "("))
      return defer_unsupported_asm(p, open, assembly);
    c_value_t dest = parse_expr(p, 1);
    if (dest.value < 0 ||
        (!c_type_is_pointer(dest.type) && !c_type_is_integer(dest.type)) ||
        !expect(p, ")") || !expect(p, ":"))
      return defer_unsupported_asm(p, open, assembly);
    size_t clobber_end = p->pos;
    if (!inline_asm_clobber_list_is(p, &clobber_end))
      return defer_unsupported_asm(p, open, assembly);
    p->pos = clobber_end;
    if (!expect(p, ")") || !expect(p, ";"))
      return defer_unsupported_asm(p, open, assembly);
    free(assembly);
    return emit(p,
                (nyr_inst_t){.op = NYIR_INLINE_ASM,
                             .dst = -1,
                             .a = source.value,
                             .b = dest.value,
                             .imm = -168,
                             .symbol = usercopy_movnti_symbol,
                             .effects = NYIR_EFFECT_WRITE_MEMORY |
                                        NYIR_EFFECT_VOLATILE},
                NULL);
  }
  if (!asm_goto && ip_fast_csum_template) {
    c_local_t *sum_local = NULL;
    int output_slots[3] = {-1, -1, -1};
    c_type_t output_types[3] = {C_BAD, C_BAD, C_BAD};
    for (size_t i = 0; i < 3; ++i) {
      if ((i == 0 && !expect(p, ":")) || (i != 0 && !expect(p, ",")) ||
          !parse_inline_asm_operand_name(p) ||
          !inline_asm_string_sequence_is(p, "=r", &p->pos) || !expect(p, "("))
        return defer_unsupported_asm(p, open, assembly);
      c_value_t output = parse_expr(p, 1);
      const c_lvalue_t *lvalue =
          output.value >= 0 ? find_lvalue(p, output.value) : NULL;
      if (output.value < 0 || !lvalue || !lvalue->is_local ||
          lvalue->local_slot < 0 || !expect(p, ")"))
        return defer_unsupported_asm(p, open, assembly);
      output_slots[i] = lvalue->local_slot;
      output_types[i] = output.type;
      if (i == 0)
        sum_local = find_local_slot(p, (size_t)lvalue->local_slot);
    }
    if (!sum_local || (sum_local->type != C_I32 && sum_local->type != C_U32) ||
        !c_type_is_pointer(output_types[1]) ||
        !c_type_is_integer(output_types[2]) || !expect(p, ":"))
      return defer_unsupported_asm(p, open, assembly);
    static const char *const tied_constraints[] = {"1", "2"};
    int input_values[2] = {-1, -1};
    for (size_t i = 0; i < 2; ++i) {
      if ((i && !expect(p, ",")) || !parse_inline_asm_operand_name(p) ||
          !inline_asm_string_sequence_is(p, tied_constraints[i], &p->pos) ||
          !expect(p, "("))
        return defer_unsupported_asm(p, open, assembly);
      c_value_t input = parse_expr(p, 1);
      const c_lvalue_t *lvalue =
          input.value >= 0 ? find_lvalue(p, input.value) : NULL;
      if (input.value < 0 || !lvalue || !lvalue->is_local ||
          lvalue->local_slot != output_slots[i + 1] || !expect(p, ")") ||
          (i == 0 ? !c_type_is_pointer(input.type)
                  : !c_type_is_integer(input.type)))
        return defer_unsupported_asm(p, open, assembly);
      input_values[i] = input.value;
    }
    size_t clobber_end = p->pos;
    if (!expect(p, ":") ||
        !inline_asm_string_sequence_is(p, "memory", &clobber_end) ||
        !tok_is(p, clobber_end, ")"))
      return defer_unsupported_asm(p, open, assembly);
    p->pos = clobber_end;
    if (!expect(p, ")") || !expect(p, ";"))
      return defer_unsupported_asm(p, open, assembly);
    int result = emit_value(p, (nyr_inst_t){.op = NYIR_INLINE_ASM,
                                            .dst = -1,
                                            .a = input_values[0],
                                            .b = input_values[1],
                                            .imm = -176,
                                            .symbol = "ip_fast_csum"});
    bool ok = result >= 0 && lower_local_store(p, sum_local, result);
    free(assembly);
    return ok;
  }

  bool kvm_vmmcall_alternative =
      INLINE_ASM_HAS("# ALT: oldinstr") && INLINE_ASM_HAS("vmcall") &&
      INLINE_ASM_HAS("vmmcall") && !INLINE_ASM_HAS("movw %[port]") &&
      INLINE_ASM_HAS(".altinstructions");
  bool vmware_alt2_template =
      INLINE_ASM_HAS("# ALT: oldinstr") && INLINE_ASM_HAS("vmcall") &&
      INLINE_ASM_HAS("vmmcall") && INLINE_ASM_HAS("movw %[port]") &&
      INLINE_ASM_HAS("inl (%%dx), %%eax") && INLINE_ASM_HAS(".altinstructions");
  bool vmware_port_alternative = INLINE_ASM_HAS("# ALT: oldinstr") &&
                                 INLINE_ASM_HAS("movw %[port], %%dx") &&
                                 INLINE_ASM_HAS("inl (%%dx), %%eax") &&
                                 INLINE_ASM_HAS(".altinstructions");
  const char *vmware_call =
      vmware_alt2_template      ? "vmware_alt2"
      : kvm_vmmcall_alternative ? "kvm_vmmcall_alt"
      : assembly_len == sizeof("vmcall") - 1 &&
              memcmp(assembly, "vmcall", sizeof("vmcall") - 1) == 0
          ? "vmcall"
      : assembly_len == sizeof("vmmcall") - 1 &&
              memcmp(assembly, "vmmcall", sizeof("vmmcall") - 1) == 0
          ? "vmmcall"
      : (vmware_port_alternative ||
         (assembly_len == sizeof("movw %[port], %%dx; inl (%%dx), %%eax") - 1 &&
          memcmp(assembly, "movw %[port], %%dx; inl (%%dx), %%eax",
                 sizeof("movw %[port], %%dx; inl (%%dx), %%eax") - 1) == 0))
          ? "vmware_port"
          : NULL;
  if (!asm_goto && vmware_call) {
    if (strcmp(vmware_call, "vmware_alt2") == 0) {
      static const char *const output_constraints[] = {"=a", "=b", "=c",
                                                       "=d", "=S", "=D"};
      static const char *const input_constraints[] = {"a", "b", "c",
                                                      "d", "S", "D"};
      int inputs[6] = {-1, -1, -1, -1, -1, -1};
      int outputs[5] = {-1, -1, -1, -1, -1};
      int result_slot = -1;
      size_t output_count = 0;
      bool output_seen[6] = {false, false, false, false, false, false};
      int zero = emit_value(p, (nyr_inst_t){.op = NYIR_CONST_I64, .dst = -1});
      if (zero < 0 || !expect(p, ":")) {
        free(assembly);
        return false;
      }
      while (output_count < 6 && !tok_is(p, p->pos, ":")) {
        if ((output_count && !expect(p, ",")) ||
            !parse_inline_asm_operand_name(p)) {
          free(assembly);
          return false;
        }
        size_t i = 0;
        for (; i < 6; ++i) {
          c_parser_t probe = *p;
          size_t end = probe.pos;
          if (inline_asm_string_sequence_is(&probe, output_constraints[i],
                                            &end) &&
              tok_is(&probe, end, "(")) {
            p->pos = end;
            break;
          }
        }
        if (i == 6 || output_seen[i] || !expect(p, "(")) {
          free(assembly);
          return false;
        }
        output_seen[i] = true;
        c_value_t output_value = parse_expr(p, 1);
        const c_lvalue_t *lvalue =
            output_value.value >= 0 ? find_lvalue(p, output_value.value) : NULL;
        if (output_value.value < 0 || !lvalue ||
            !c_type_is_integer(output_value.type) || !expect(p, ")")) {
          free(assembly);
          return false;
        }
        if (i == 0) {
          result_slot = (int)lvalue->local_slot;
          if (!lvalue->is_local || result_slot < 0) {
            free(assembly);
            return false;
          }
        } else {
          int address = lvalue->address;
          if (address < 0 && lvalue->is_local)
            address = emit_value(
                p, (nyr_inst_t){.op = NYIR_ADDR_LOCAL,
                                .dst = -1,
                                .imm = lvalue->local_slot,
                                .scalar_class = NYIR_SCALAR_I64_RAW});
          if (address < 0) {
            free(assembly);
            return false;
          }
          outputs[i - 1] = address;
        }
        ++output_count;
      }
      if (output_count == 0 || !expect(p, ":")) {
        free(assembly);
        return false;
      }
      ny_ctok_t port_name;
      if (!parse_named_inline_asm_operand(p, &port_name) ||
          !inline_asm_name_matches(port_name, (const unsigned char *)"port",
                                   4) ||
          !inline_asm_string_sequence_is(p, "i", &p->pos) || !expect(p, "(")) {
        free(assembly);
        return false;
      }
      c_value_t port = parse_expr(p, 1);
      int64_t port_number = 0;
      if (port.value < 0 || !c_type_is_integer(port.type) ||
          !c_ir_nonnegative_constant(p, port.value, &port_number) ||
          port_number > UINT16_MAX || !expect(p, ")") || !expect(p, ",")) {
        free(assembly);
        return false;
      }
      for (size_t i = 0; i < 6 && !tok_is(p, p->pos, ":"); ++i) {
        if ((i && !expect(p, ",")) || !parse_inline_asm_operand_name(p) ||
            !inline_asm_string_sequence_is(p, input_constraints[i], &p->pos) ||
            !expect(p, "(")) {
          free(assembly);
          return false;
        }
        c_value_t input = parse_expr(p, 1);
        if (input.value < 0 || !c_type_is_integer(input.type) ||
            !expect(p, ")")) {
          free(assembly);
          return false;
        }
        inputs[i] = input.value;
      }
      if (inputs[1] < 0 || inputs[2] < 0 || !expect(p, ":")) {
        free(assembly);
        return false;
      }
      size_t clobber_end = p->pos;
      if (!inline_asm_clobber_list_is(p, &clobber_end)) {
        free(assembly);
        return false;
      }
      bool has_cc = false;
      bool has_memory = false;
      for (size_t at = p->pos; at < clobber_end;) {
        c_parser_t probe = *p;
        probe.pos = at;
        size_t next = at;
        if (inline_asm_string_sequence_is(&probe, "cc", &next))
          has_cc = true;
        else if (inline_asm_string_sequence_is(&probe, "memory", &next))
          has_memory = true;
        else {
          static const char *const registers[] = {
              "rax", "rbx", "rcx", "rdx", "rsi", "rdi", "rbp", "rsp",
              "di",  "si",  "r8",  "r9",  "r10", "r11", "r12", "r13",
              "r14", "r15", "eax", "ebx", "ecx", "edx", "esi", "edi"};
          for (size_t r = 0; r < sizeof(registers) / sizeof(registers[0]); ++r)
            if (inline_asm_string_sequence_is(&probe, registers[r], &next))
              break;
        }
        at = next;
        if (tok_is(p, at, ","))
          ++at;
      }
      p->pos = clobber_end;
      if (!has_cc || !has_memory || !expect(p, ")") || !expect(p, ";")) {
        free(assembly);
        return false;
      }
      int args[10] = {inputs[2],
                      inputs[1],
                      inputs[3] >= 0 ? inputs[3] : zero,
                      inputs[4] >= 0 ? inputs[4] : zero,
                      inputs[5] >= 0 ? inputs[5] : zero,
                      zero};
      for (size_t i = 0; i < 5; ++i)
        args[5 + i] = outputs[i] >= 0 ? outputs[i] : zero;
      nyr_inst_t call = {.op = NYIR_CALL,
                         .dst = -1,
                         .a = args[0],
                         .b = args[1],
                         .c = args[2],
                         .d = args[3],
                         .e = args[4],
                         .f = args[5],
                         .extra_args = &args[6],
                         .extra_args_len = 4,
                         .imm = 10,
                         .flags = NYIR_INST_F_EXTERN,
                         .effects = NYIR_EFFECT_CALL,
                         .symbol = "vmware_hypercall_slow"};
      for (size_t i = 0; i < 10; ++i)
        nyr_call_arg_class_set(call.arg_scalar_classes, i, NYIR_SCALAR_I64_RAW);
      int result = emit_value(p, call);
      free(assembly);
      if (result < 0)
        return false;
      c_local_t *result_local = NULL;
      for (size_t i = 0; i < p->local_count; ++i)
        if (p->locals[i].slot == (size_t)result_slot) {
          result_local = &p->locals[i];
          break;
        }
      return result_local && lower_local_store(p, result_local, result);
    }
    bool kvm_vmmcall = strcmp(vmware_call, "kvm_vmmcall_alt") == 0;
    if (strcmp(vmware_call, "vmmcall") == 0 ||
        strcmp(vmware_call, "vmcall") == 0) {
      c_parser_t probe = *p;
      if (expect(&probe, ":") &&
          inline_asm_string_sequence_is(&probe, "=a", &probe.pos) &&
          expect(&probe, "(") && tok_ident(*current(&probe))) {
        ++probe.pos;
        kvm_vmmcall = expect(&probe, ")") && tok_is(&probe, probe.pos, ":");
      }
    }
    if (kvm_vmmcall) {
      if (!expect(p, ":") || !inline_asm_string_sequence_is(p, "=a", &p->pos) ||
          !expect(p, "(")) {
        free(assembly);
        return false;
      }
      ny_ctok_t output_name;
      if (!parse_name(p, &output_name) || !expect(p, ")") || !expect(p, ":")) {
        free(assembly);
        return false;
      }
      c_local_t *output = find_local(p, output_name);
      if (!output || output->array_rank || output->is_const ||
          !c_type_is_integer(output->type)) {
        free(assembly);
        return false;
      }
      static const char *const constraints[] = {"a", "b", "c", "d", "S"};
      int values[5] = {-1, -1, -1, -1, -1};
      size_t input_count = 0;
      while (input_count < 5 && !tok_is(p, p->pos, ":")) {
        if ((input_count && !expect(p, ",")) ||
            !inline_asm_string_sequence_is(p, constraints[input_count],
                                           &p->pos) ||
            !expect(p, "(")) {
          free(assembly);
          return false;
        }
        c_value_t input = parse_expr(p, 1);
        if (input.value < 0 || !c_type_is_integer(input.type) ||
            !expect(p, ")")) {
          free(assembly);
          return false;
        }
        values[input_count++] = input.value;
      }
      if (input_count == 0 || !expect(p, ":") ||
          !inline_asm_string_sequence_is(p, "memory", &p->pos) ||
          !expect(p, ")") || !expect(p, ";")) {
        free(assembly);
        return false;
      }
      char lowered[64];
      int symbol_len =
          snprintf(lowered, sizeof(lowered),
                   kvm_vmmcall_alternative ? "kvm_vmmcall;slot=%zu;alt=271"
                                           : "kvm_vmmcall;slot=%zu",
                   output->slot);
      const char *symbol =
          symbol_len < 0 || (size_t)symbol_len >= sizeof(lowered)
              ? NULL
              : nyr_func_own_symbol_copy(p->out, lowered);
      free(assembly);
      if (!symbol)
        return false;
      return emit(
          p,
          (nyr_inst_t){.op = NYIR_INLINE_ASM,
                       .dst = -1,
                       .a = values[0],
                       .b = values[1],
                       .c = values[2],
                       .d = values[3],
                       .e = values[4],
                       .f = -1,
                       .imm = -142,
                       .symbol = symbol,
                       .effects = NYIR_EFFECT_CALL | NYIR_EFFECT_READ_MEMORY |
                                  NYIR_EFFECT_WRITE_MEMORY |
                                  NYIR_EFFECT_CONTROL | NYIR_EFFECT_VOLATILE},
          NULL);
    }
    static const char *const output_constraints[] = {"=a", "=b", "=c",
                                                     "=d", "=S", "=D"};
    static const char *const input_constraints[] = {"a", "b", "c",
                                                    "d", "S", "D"};
    int values[6] = {-1, -1, -1, -1, -1, -1};
    int outputs[6];
    char lowered[160];
    bool vmware_port = strcmp(vmware_call, "vmware_port") == 0;
    int64_t port_number = 0;
    if (!expect(p, ":")) {
      free(assembly);
      return false;
    }
    for (size_t i = 0; i < 6; ++i) {
      if ((i && !expect(p, ",")) || !parse_inline_asm_operand_name(p) ||
          !inline_asm_string_sequence_is(p, output_constraints[i], &p->pos) ||
          !expect(p, "(")) {
        free(assembly);
        return false;
      }
      ny_ctok_t output_name;
      if (!parse_name(p, &output_name) || !expect(p, ")")) {
        free(assembly);
        return false;
      }
      c_local_t *output = find_local(p, output_name);
      if (!output || output->array_rank || output->is_const ||
          !c_type_is_integer(output->type)) {
        free(assembly);
        return false;
      }
      outputs[i] =
          emit_value(p, (nyr_inst_t){.op = NYIR_ADDR_LOCAL,
                                     .dst = -1,
                                     .imm = (int64_t)output->slot,
                                     .scalar_class = NYIR_SCALAR_I64_RAW});
      if (outputs[i] < 0) {
        free(assembly);
        return false;
      }
    }
    if (vmware_port) {
      ny_ctok_t port_name;
      if (!expect(p, ":") || !parse_named_inline_asm_operand(p, &port_name) ||
          !inline_asm_name_matches(port_name, (const unsigned char *)"port",
                                   4) ||
          !inline_asm_string_sequence_is(p, "i", &p->pos) || !expect(p, "(")) {
        free(assembly);
        return false;
      }
      c_value_t port = parse_expr(p, 1);
      if (port.value < 0 || !c_type_is_integer(port.type) || !expect(p, ")") ||
          !expect(p, ",") ||
          !c_ir_nonnegative_constant(p, port.value, &port_number) ||
          port_number > UINT16_MAX) {
        free(assembly);
        return false;
      }
    }
    for (size_t i = 0; i < 6; ++i) {
      if ((!vmware_port && i == 0 && !expect(p, ":")) ||
          (i > 0 && !expect(p, ",")) || !parse_inline_asm_operand_name(p) ||
          !inline_asm_string_sequence_is(p, input_constraints[i], &p->pos) ||
          !expect(p, "(")) {
        free(assembly);
        return false;
      }
      c_value_t input = parse_expr(p, 1);
      if (input.value < 0 || !c_type_is_integer(input.type) ||
          !expect(p, ")")) {
        free(assembly);
        return false;
      }
      values[i] = input.value;
    }
    if (!expect(p, ":")) {
      free(assembly);
      return false;
    }
    bool cc_clobber = inline_asm_string_sequence_is(p, "cc", &p->pos);
    if (!cc_clobber || !expect(p, ",")) {
      free(assembly);
      return false;
    }
    bool memory_clobber = inline_asm_string_sequence_is(p, "memory", &p->pos);
    if (!memory_clobber || !expect(p, ")") || !expect(p, ";")) {
      free(assembly);
      return false;
    }
    int symbol_len =
        vmware_port
            ? snprintf(lowered, sizeof(lowered), "vmware_port;port=%lld%s",
                       (long long)port_number,
                       vmware_port_alternative ? ";alt=274" : "")
            : snprintf(lowered, sizeof(lowered), "%s", vmware_call);
    const char *symbol = symbol_len < 0 || (size_t)symbol_len >= sizeof(lowered)
                             ? NULL
                             : nyr_func_own_symbol_copy(p->out, lowered);
    free(assembly);
    if (!symbol)
      return false;
    return emit(
        p,
        (nyr_inst_t){.op = NYIR_INLINE_ASM,
                     .dst = -1,
                     .a = values[0],
                     .b = values[1],
                     .c = values[2],
                     .d = values[3],
                     .e = values[4],
                     .f = values[5],
                     .extra_args = outputs,
                     .extra_args_len = 6,
                     .imm = vmware_port ? -141 : -140,
                     .symbol = symbol,
                     .effects = NYIR_EFFECT_CALL | NYIR_EFFECT_READ_MEMORY |
                                NYIR_EFFECT_WRITE_MEMORY | NYIR_EFFECT_CONTROL |
                                NYIR_EFFECT_VOLATILE},
        NULL);
  }

  bool rep_movs_alternative_template =
      !asm_goto && INLINE_ASM_HAS("# ALT: oldinstr") &&
      INLINE_ASM_HAS("rep movsb") &&
      INLINE_ASM_HAS("call rep_movs_alternative") &&
      INLINE_ASM_HAS(".altinstructions") &&
      INLINE_ASM_HAS(".pushsection __ex_table");
  if (rep_movs_alternative_template) {
    int values[3] = {-1, -1, -1};
    int addresses[3] = {-1, -1, -1};
    static const char *const constraints[] = {"+c", "+D", "+S"};
    for (size_t i = 0; i < 3; ++i) {
      if ((i == 0 && !expect(p, ":")) || (i && !expect(p, ",")) ||
          !parse_inline_asm_operand_name(p) ||
          !inline_asm_string_sequence_is(p, constraints[i], &p->pos) ||
          !expect(p, "(")) {
        free(assembly);
        return false;
      }
      c_value_t output = parse_expr(p, 1);
      const c_lvalue_t *lvalue =
          output.value >= 0 ? find_lvalue(p, output.value) : NULL;
      if (output.value < 0 || !lvalue ||
          (i == 0 ? !c_type_is_integer(output.type)
                  : !c_type_is_pointer(output.type)) ||
          !expect(p, ")")) {
        if (!p->err || !p->err_len || !p->err[0])
          fail(p,
               "REP MOVSB inline assembly requires local read/write operands");
        free(assembly);
        return false;
      }
      int address = lvalue->address;
      if (address < 0 && lvalue->is_local)
        address =
            emit_value(p, (nyr_inst_t){.op = NYIR_ADDR_LOCAL,
                                       .dst = -1,
                                       .imm = lvalue->local_slot,
                                       .scalar_class = NYIR_SCALAR_I64_RAW});
      if (address < 0) {
        fail(p,
             "REP MOVSB inline assembly could not address an output operand");
        free(assembly);
        return false;
      }
      values[i] = output.value;
      addresses[i] = address;
    }
    if (!expect(p, ",") || !inline_asm_string_sequence_is(p, "+r", &p->pos) ||
        !expect(p, "(")) {
      free(assembly);
      return false;
    }
    c_value_t stack_pointer = parse_expr(p, 1);
    if (stack_pointer.value < 0 || !c_type_is_integer(stack_pointer.type) ||
        !expect(p, ")") || !expect(p, ":") || !expect(p, ":") ||
        !inline_asm_string_sequence_is(p, "memory", &p->pos) ||
        !expect(p, ",") || !inline_asm_string_sequence_is(p, "rax", &p->pos) ||
        !expect(p, ")") || !expect(p, ";")) {
      if (!p->err || !p->err_len || !p->err[0])
        fail(p, "REP MOVSB inline assembly requires the x86 stack-pointer call "
                "constraint");
      free(assembly);
      return false;
    }
    unescape_inline_asm_percents(assembly, &assembly_len);
    if (!append_asm_bytes(&assembly, &assembly_len, &assembly_cap, "\0", 1)) {
      free(assembly);
      return false;
    }
    const char *raw = nyr_func_own_symbol_copy(p->out, (const char *)assembly);
    free(assembly);
    if (!raw)
      return false;
    return emit(
        p,
        (nyr_inst_t){.op = NYIR_INLINE_ASM,
                     .dst = -1,
                     .a = values[1],
                     .b = values[2],
                     .c = values[0],
                     .d = addresses[1],
                     .e = addresses[2],
                     .f = addresses[0],
                     .imm = -138,
                     .symbol = raw,
                     .effects = NYIR_EFFECT_CALL | NYIR_EFFECT_READ_MEMORY |
                                NYIR_EFFECT_WRITE_MEMORY | NYIR_EFFECT_CONTROL |
                                NYIR_EFFECT_VOLATILE},
        NULL);
  }

  if (!asm_goto && assembly_len == sizeof("invlpg (%0)") - 1 &&
      memcmp(assembly, "invlpg (%0)", sizeof("invlpg (%0)") - 1) == 0) {
    if (!expect(p, ":") || !expect(p, ":") ||
        !inline_asm_string_sequence_is(p, "r", &p->pos) || !expect(p, "(")) {
      free(assembly);
      return false;
    }
    c_value_t address = parse_expr(p, 1);
    if (address.value < 0 ||
        (!c_type_is_integer(address.type) &&
         !c_type_is_pointer(address.type)) ||
        !expect(p, ")") || !expect(p, ":") ||
        !inline_asm_string_sequence_is(p, "memory", &p->pos) ||
        !expect(p, ")") || !expect(p, ";")) {
      free(assembly);
      return false;
    }
    free(assembly);
    return emit(p,
                (nyr_inst_t){.op = NYIR_INLINE_ASM,
                             .dst = -1,
                             .a = address.value,
                             .b = -1,
                             .c = -1,
                             .d = -1,
                             .e = -1,
                             .f = -1,
                             .imm = -139,
                             .symbol = "invlpg",
                             .effects = NYIR_EFFECT_READ_MEMORY |
                                        NYIR_EFFECT_WRITE_MEMORY |
                                        NYIR_EFFECT_CONTROL |
                                        NYIR_EFFECT_VOLATILE},
                NULL);
  }

  static const struct {
    const char *assembly;
    const char *symbol;
    int64_t lowering;
    bool output;
  } descriptor_table_asm[] = {
      {"sgdt %0", "sgdt %0", -71, true},   {"sidt %0", "sidt %0", -72, true},
      {"sldt %0", "sldt %0", -152, true},  {"lgdt %0", "lgdt %0", -73, false},
      {"lidt %0", "lidt %0", -74, false},  {"sgdtl %0", "sgdtl %0", -71, true},
      {"sidtl %0", "sidtl %0", -72, true}, {"lgdtl %0", "lgdtl %0", -73, false},
      {"lidtl %0", "lidtl %0", -74, false}};
  for (size_t i = 0; !asm_goto && i < sizeof(descriptor_table_asm) /
                                          sizeof(descriptor_table_asm[0]);
       ++i) {
    if (assembly_len != strlen(descriptor_table_asm[i].assembly) ||
        memcmp(assembly, descriptor_table_asm[i].assembly, assembly_len) != 0)
      continue;
    c_value_t operand;
    if (descriptor_table_asm[i].output) {
      if (!expect(p, ":") || !inline_asm_string_sequence_is(p, "=m", &p->pos) ||
          !expect(p, "(")) {
        if (!p->err || !p->err_len || !p->err[0])
          fail(p, "invalid descriptor-table inline-asm output constraint");
        free(assembly);
        return false;
      }
      operand = parse_expr(p, 1);
      if (operand.value < 0 || !expect(p, ")") || !expect(p, ")") ||
          !expect(p, ";")) {
        if (!p->err || !p->err_len || !p->err[0])
          fail(p, "invalid descriptor-table inline-asm output operand");
        free(assembly);
        return false;
      }
    } else {
      if (!expect(p, ":") || !expect(p, ":") ||
          !inline_asm_string_sequence_is(p, "m", &p->pos) || !expect(p, "(")) {
        free(assembly);
        return false;
      }
      operand = parse_expr(p, 1);
      if (operand.value < 0 || !expect(p, ")") || !expect(p, ")") ||
          !expect(p, ";")) {
        free(assembly);
        return false;
      }
    }
    /* Aggregate expressions already carry their storage address. In *p
     * that value can also identify the pointer variable's own lvalue. */
    const c_lvalue_t *lvalue =
        operand.type == C_AGG ? NULL : find_asm_memory_lvalue(p, operand.value);
    if (lvalue && lvalue->is_local) {
      c_local_t *local = find_local_slot(p, lvalue->local_slot);
      if (!local || (descriptor_table_asm[i].output && local->is_const)) {
        free(assembly);
        return false;
      }
      local->address_taken = true;
      if (descriptor_table_asm[i].output)
        local->has_integer_constant = false;
    }
    int address = lvalue && lvalue->address >= 0 ? lvalue->address
                  : operand.type == C_AGG        ? operand.value
                                                 : -1;
    if (address < 0) {
      if (!p->err || !p->err_len || !p->err[0]) {
        fail(p, "descriptor-table inline-asm output must be addressable");
      }
      free(assembly);
      return false;
    }
    free(assembly);
    return emit(p,
                (nyr_inst_t){.op = NYIR_INLINE_ASM,
                             .dst = -1,
                             .a = address,
                             .b = -1,
                             .c = -1,
                             .d = -1,
                             .e = -1,
                             .f = -1,
                             .imm = descriptor_table_asm[i].lowering,
                             .symbol = descriptor_table_asm[i].symbol,
                             .effects = NYIR_EFFECT_READ_MEMORY |
                                        NYIR_EFFECT_WRITE_MEMORY |
                                        NYIR_EFFECT_CONTROL |
                                        NYIR_EFFECT_VOLATILE},
                NULL);
  }

  if (!asm_goto && assembly_len == sizeof("verw %[ds]") - 1 &&
      memcmp(assembly, "verw %[ds]", sizeof("verw %[ds]") - 1) == 0) {
    ny_ctok_t operand_name;
    if (!expect(p, ":") || !expect(p, ":") ||
        !parse_named_inline_asm_operand(p, &operand_name) ||
        !inline_asm_name_matches(operand_name, (const unsigned char *)"ds",
                                 2) ||
        !inline_asm_string_sequence_is(p, "m", &p->pos) || !expect(p, "(")) {
      free(assembly);
      return false;
    }
    ny_ctok_t value_name;
    if (!parse_name(p, &value_name) || !expect(p, ")") || !expect(p, ":") ||
        !inline_asm_string_sequence_is(p, "cc", &p->pos) || !expect(p, ")") ||
        !expect(p, ";")) {
      free(assembly);
      return false;
    }
    c_local_t *local = find_local(p, value_name);
    c_global_t *global = local ? NULL : find_global(p, value_name);
    c_type_t operand_type = local ? local->type : global ? global->type : C_BAD;
    int address = -1;
    if (!c_type_is_integer(operand_type)) {
      free(assembly);
      return false;
    }
    if (local && !local->static_name.len)
      address =
          emit_value(p, (nyr_inst_t){.op = NYIR_ADDR_LOCAL,
                                     .dst = -1,
                                     .imm = (int64_t)local->slot,
                                     .scalar_class = NYIR_SCALAR_I64_RAW});
    else {
      ny_ctok_t symbol_name = local ? local->static_name : global->name;
      static const char prefix[] = "__nyc_data_";
      size_t symbol_len = sizeof(prefix) - 1 + symbol_name.len;
      char *symbol = malloc(symbol_len + 1);
      if (!symbol) {
        free(assembly);
        return false;
      }
      memcpy(symbol, prefix, sizeof(prefix) - 1);
      memcpy(symbol + sizeof(prefix) - 1, symbol_name.start, symbol_name.len);
      symbol[symbol_len] = '\0';
      address =
          emit_value(p, (nyr_inst_t){.op = NYIR_ADDR_SYMBOL,
                                     .dst = -1,
                                     .symbol = symbol,
                                     .scalar_class = NYIR_SCALAR_I64_RAW});
      free(symbol);
    }
    free(assembly);
    return address >= 0 &&
           emit(p,
                (nyr_inst_t){.op = NYIR_INLINE_ASM,
                             .dst = -1,
                             .a = address,
                             .b = -1,
                             .c = -1,
                             .d = -1,
                             .e = -1,
                             .f = -1,
                             .imm = -114,
                             .symbol = "verw_mem",
                             .effects = NYIR_EFFECT_READ_MEMORY |
                                        NYIR_EFFECT_CONTROL |
                                        NYIR_EFFECT_VOLATILE},
                NULL);
  }

  bool alt_reloc_selftest_template =
      !asm_goto && INLINE_ASM_HAS("# ALT: oldinstr") &&
      INLINE_ASM_HAS("lea %[mem], %%rdi; call __alt_reloc_selftest;") &&
      INLINE_ASM_HAS(".altinstructions");
  if (alt_reloc_selftest_template) {
    if (!expect(p, ":") || !inline_asm_string_sequence_is(p, "+r", &p->pos) ||
        !expect(p, "(")) {
      free(assembly);
      return false;
    }
    c_value_t stack_pointer = parse_expr(p, 1);
    const c_lvalue_t *stack_lvalue =
        stack_pointer.value >= 0 ? find_lvalue(p, stack_pointer.value) : NULL;
    if (stack_pointer.value < 0 || !c_type_is_integer(stack_pointer.type) ||
        !expect(p, ")") ||
        (stack_lvalue && !discard_lvalue_read(p, stack_lvalue)) ||
        !expect(p, ":")) {
      free(assembly);
      return false;
    }
    ny_ctok_t memory_name;
    if (!parse_named_inline_asm_operand(p, &memory_name) ||
        !inline_asm_name_matches(memory_name, (const unsigned char *)"mem",
                                 3) ||
        !inline_asm_string_sequence_is(p, "m", &p->pos) || !expect(p, "(")) {
      free(assembly);
      return false;
    }
    ny_ctok_t memory_symbol;
    if (!parse_name(p, &memory_symbol) ||
        !inline_asm_name_matches(
            memory_symbol, (const unsigned char *)"__alt_reloc_selftest_addr",
            sizeof("__alt_reloc_selftest_addr") - 1) ||
        !find_global(p, memory_symbol) || !expect(p, ")") || !expect(p, ":") ||
        !inline_asm_string_sequence_is(p, "rdi", &p->pos) || !expect(p, ")") ||
        !expect(p, ";")) {
      free(assembly);
      return false;
    }
    inline_asm_named_value_t memory_input = {
        .name = memory_name, .replacement = "__alt_reloc_selftest_addr(%rip)"};
    if (!substitute_inline_asm_named_operands(&assembly, &assembly_len,
                                              &assembly_cap, (ny_ctok_t){0},
                                              NULL, &memory_input, 1)) {
      free(assembly);
      return false;
    }
    unescape_inline_asm_percents(assembly, &assembly_len);
    if (!append_asm_bytes(&assembly, &assembly_len, &assembly_cap, "\0", 1)) {
      free(assembly);
      return false;
    }
    const char *raw = nyr_func_own_symbol_copy(p->out, (const char *)assembly);
    free(assembly);
    if (!raw)
      return false;
    return emit(
        p,
        (nyr_inst_t){.op = NYIR_INLINE_ASM,
                     .dst = -1,
                     .a = -1,
                     .b = -1,
                     .c = -1,
                     .d = -1,
                     .e = -1,
                     .f = -1,
                     .imm = -137,
                     .symbol = raw,
                     .effects = NYIR_EFFECT_CALL | NYIR_EFFECT_READ_MEMORY |
                                NYIR_EFFECT_WRITE_MEMORY | NYIR_EFFECT_CONTROL |
                                NYIR_EFFECT_VOLATILE},
        NULL);
  }

  if (!asm_goto &&
      memmem(assembly, assembly_len, "pushq $1f", sizeof("pushq $1f") - 1) &&
      memmem(assembly, assembly_len, "iretq", sizeof("iretq") - 1) &&
      memmem(assembly, assembly_len, "pushfq", sizeof("pushfq") - 1) &&
      memmem(assembly, assembly_len, "mov %%ss, %0",
             sizeof("mov %%ss, %0") - 1)) {
    static const char *const constraints[] = {"=&r", "+r"};
    for (size_t i = 0; i < 2; ++i) {
      if ((i == 0 && !expect(p, ":")) || (i && !expect(p, ",")) ||
          !parse_inline_asm_operand_name(p) ||
          !inline_asm_string_sequence_is(p, constraints[i], &p->pos) ||
          !expect(p, "(")) {
        free(assembly);
        return false;
      }
      c_value_t output = parse_expr(p, 1);
      const c_lvalue_t *lvalue =
          output.value >= 0 ? find_lvalue(p, output.value) : NULL;
      if (output.value < 0 || !c_type_is_integer(output.type) ||
          !expect(p, ")") || (lvalue && !discard_lvalue_read(p, lvalue))) {
        free(assembly);
        return false;
      }
    }
    if (!expect(p, ":") || !expect(p, ":") ||
        !inline_asm_string_sequence_is(p, "cc", &p->pos) || !expect(p, ",") ||
        !inline_asm_string_sequence_is(p, "memory", &p->pos) ||
        !expect(p, ")") || !expect(p, ";")) {
      free(assembly);
      return false;
    }
    free(assembly);
    return emit(
        p,
        (nyr_inst_t){.op = NYIR_INLINE_ASM,
                     .dst = -1,
                     .a = -1,
                     .b = -1,
                     .c = -1,
                     .d = -1,
                     .e = -1,
                     .f = -1,
                     .imm = -59,
                     .symbol = "iret_to_self",
                     .effects = NYIR_EFFECT_CONTROL | NYIR_EFFECT_VOLATILE},
        NULL);
  }

  static const char trace_data_template[] = "movq $1f,%0\n"
                                            ".section .tracedata,\"a\"\n"
                                            "1:\t.word %c1\n\t"
                                            ".quad %c2\n"
                                            ".previous";
  if (!asm_goto && p->abi == NYC_ABI_SYSV_X86_64 &&
      inline_asm_template_ignoring_whitespace(assembly, assembly_len,
                                              trace_data_template)) {
    if (!expect(p, ":") || !inline_asm_string_sequence_is(p, "=r", &p->pos) ||
        !expect(p, "("))
      return defer_unsupported_asm(p, open, assembly);
    c_value_t output = parse_expr(p, 1);
    const c_lvalue_t *output_lvalue =
        output.value >= 0 ? find_lvalue(p, output.value) : NULL;
    c_local_t *output_local =
        output_lvalue && output_lvalue->is_local &&
                output_lvalue->local_slot >= 0
            ? find_local_slot(p, (size_t)output_lvalue->local_slot)
            : NULL;
    if (!output_local || !c_type_is_pointer(output_local->type) ||
        !expect(p, ")") || !expect(p, ":") ||
        !inline_asm_string_sequence_is(p, "i", &p->pos) || !expect(p, "("))
      return defer_unsupported_asm(p, open, assembly);
    bool previous_constant_context = p->inline_asm_constant_context;
    p->inline_asm_constant_context = true;
    int64_t line = 0;
    bool line_ok = parse_global_const_expr(p, &p->pos, &line);
    p->inline_asm_constant_context = previous_constant_context;
    if (!line_ok || !expect(p, ")") || !expect(p, ",") ||
        !inline_asm_string_sequence_is(p, "i", &p->pos) || !expect(p, "("))
      return defer_unsupported_asm(p, open, assembly);
    size_t string_global_index = p->global_count;
    c_value_t file = parse_expr(p, 1);
    if (file.value < 0 || file.type != C_CHAR_PTR ||
        p->global_count != string_global_index + 1 || !expect(p, ")") ||
        !expect(p, ")") || !expect(p, ";"))
      return defer_unsupported_asm(p, open, assembly);
    c_global_t *file_global = &p->globals[string_global_index];
    if (file_global->name.len < sizeof("__nyc_string_") - 1 ||
        memcmp(file_global->name.start, "__nyc_string_",
               sizeof("__nyc_string_") - 1) != 0 ||
        p->global_count >= p->global_capacity || line < 0 ||
        line > UINT16_MAX) {
      free(assembly);
      fail(p, "invalid x86 PM trace metadata operands");
      return false;
    }
    char generated_name[48];
    int generated_len = snprintf(generated_name, sizeof(generated_name),
                                 "__nyc_static_%zu", p->global_count);
    char target_symbol[96];
    int target_len =
        snprintf(target_symbol, sizeof(target_symbol), "__nyc_data_%.*s",
                 (int)file_global->name.len, file_global->name.start);
    unsigned char *data_bytes = calloc(10, 1);
    char *owned_name =
        generated_len > 0 && (size_t)generated_len < sizeof(generated_name)
            ? strdup(generated_name)
            : NULL;
    char *owned_target =
        target_len > 0 && (size_t)target_len < sizeof(target_symbol)
            ? strdup(target_symbol)
            : NULL;
    char *section = strdup(".tracedata");
    c_global_reloc_t *relocs = calloc(1, sizeof(*relocs));
    if (!data_bytes || !owned_name || !owned_target || !section || !relocs) {
      free(data_bytes);
      free(owned_name);
      free(owned_target);
      free(section);
      free(relocs);
      free(assembly);
      fail(p, "out of memory creating x86 PM trace metadata");
      return false;
    }
    uint16_t trace_line = (uint16_t)line;
    memcpy(data_bytes, &trace_line, sizeof(trace_line));
    relocs[0] = (c_global_reloc_t){.offset = 2,
                                   .target = {.kind = NY_CTOK_IDENT,
                                              .start = owned_target,
                                              .len = (size_t)target_len},
                                   .owned_target = owned_target,
                                   .width = sizeof(uint64_t),
                                   .type = NYC_DATA_RELOC_ABS64};
    ny_ctok_t trace_name = {.kind = NY_CTOK_IDENT,
                            .start = owned_name,
                            .len = (size_t)generated_len};
    p->globals[p->global_count++] =
        (c_global_t){.name = trace_name,
                     .bytes = data_bytes,
                     .owned_name = owned_name,
                     .len = 10,
                     .type = C_U8,
                     .is_array = true,
                     .is_const = true,
                     .is_local = true,
                     .owner_function = p->function->name,
                     .section_flags = 2,
                     .alignment = 1,
                     .section = section,
                     .relocs = relocs,
                     .reloc_count = 1};
    char data_symbol[112];
    int symbol_len = snprintf(data_symbol, sizeof(data_symbol), "__nyc_data_%s",
                              generated_name);
    char *owned_symbol =
        symbol_len > 0 && (size_t)symbol_len < sizeof(data_symbol)
            ? strdup(data_symbol)
            : NULL;
    if (!owned_symbol) {
      free(assembly);
      fail(p, "out of memory naming x86 PM trace metadata");
      return false;
    }
    int address =
        emit_value(p, (nyr_inst_t){.op = NYIR_ADDR_SYMBOL,
                                   .dst = -1,
                                   .symbol = owned_symbol,
                                   .scalar_class = NYIR_SCALAR_I64_RAW});
    free(owned_symbol);
    bool ok = address >= 0 && lower_local_store(p, output_local, address);
    free(assembly);
    return ok;
  }

  static const char *const movntdqa_templates[] = {
      "movntdqa (%0), %%xmm0\n"
      "movntdqa 16(%0), %%xmm1\n"
      "movntdqa 32(%0), %%xmm2\n"
      "movntdqa 48(%0), %%xmm3\n"
      "movaps %%xmm0, (%1)\n"
      "movaps %%xmm1, 16(%1)\n"
      "movaps %%xmm2, 32(%1)\n"
      "movaps %%xmm3, 48(%1)",
      "movntdqa (%0), %%xmm0\n"
      "movaps %%xmm0, (%1)",
      "movntdqa (%0), %%xmm0\n"
      "movups %%xmm0, (%1)",
      "movntdqa (%0), %%xmm0\n"
      "movntdqa 16(%0), %%xmm1\n"
      "movntdqa 32(%0), %%xmm2\n"
      "movntdqa 48(%0), %%xmm3\n"
      "movups %%xmm0, (%1)\n"
      "movups %%xmm1, 16(%1)\n"
      "movups %%xmm2, 32(%1)\n"
      "movups %%xmm3, 48(%1)",
  };
  static const char *const movntdqa_symbols[] = {
      "movntdqa_movaps_4",
      "movntdqa_movaps_1",
      "movntdqa_movups_1",
      "movntdqa_movups_4",
  };
  if (!asm_goto && p->abi == NYC_ABI_SYSV_X86_64) {
    int variant = -1;
    for (size_t i = 0;
         i < sizeof(movntdqa_templates) / sizeof(movntdqa_templates[0]); ++i)
      if (inline_asm_template_is(assembly, assembly_len,
                                 movntdqa_templates[i])) {
        variant = (int)i;
        break;
      }
    if (variant >= 0) {
      int inputs[2] = {-1, -1};
      if (!expect(p, ":") || !expect(p, ":"))
        return defer_unsupported_asm(p, open, assembly);
      static const char *const constraints[] = {"r", "r"};
      for (size_t i = 0; i < 2; ++i) {
        if ((i && !expect(p, ",")) ||
            !inline_asm_string_sequence_is(p, constraints[i], &p->pos) ||
            !expect(p, "("))
          return defer_unsupported_asm(p, open, assembly);
        c_value_t input = parse_expr(p, 1);
        if (input.value < 0 || !c_type_is_pointer(input.type) ||
            !expect(p, ")"))
          return defer_unsupported_asm(p, open, assembly);
        inputs[i] = input.value;
      }
      if (!expect(p, ":") ||
          !inline_asm_string_sequence_is(p, "memory", &p->pos) ||
          !expect(p, ")") || !expect(p, ";"))
        return defer_unsupported_asm(p, open, assembly);
      free(assembly);
      return emit(p,
                  (nyr_inst_t){.op = NYIR_INLINE_ASM,
                               .dst = -1,
                               .a = inputs[0],
                               .b = inputs[1],
                               .c = -1,
                               .d = -1,
                               .e = -1,
                               .f = -1,
                               .imm = -179,
                               .symbol = movntdqa_symbols[variant],
                               .effects = NYIR_EFFECT_READ_MEMORY |
                                          NYIR_EFFECT_WRITE_MEMORY |
                                          NYIR_EFFECT_CONTROL |
                                          NYIR_EFFECT_VOLATILE},
                  NULL);
    }
  }

  static const char xsha256_template[] = ".byte 0xf3,0x0f,0xa6,0xd0";
  if (!asm_goto && p->abi == NYC_ABI_SYSV_X86_64 &&
      inline_asm_template_is(assembly, assembly_len, xsha256_template)) {
    int addresses[3] = {-1, -1, -1};
    static const char *const constraints[] = {"+a", "+c", "+S"};
    for (size_t i = 0; i < 3; ++i) {
      if ((i == 0 && !expect(p, ":")) || (i && !expect(p, ",")) ||
          !parse_inline_asm_operand_name(p) ||
          !inline_asm_string_sequence_is(p, constraints[i], &p->pos) ||
          !expect(p, "("))
        return defer_unsupported_asm(p, open, assembly);
      c_value_t output = parse_expr(p, 1);
      const c_lvalue_t *lvalue =
          output.value >= 0 ? find_lvalue(p, output.value) : NULL;
      if (!lvalue || c_type_size(p->abi, output.type) != 8 ||
          (i < 2 ? !c_type_is_integer(output.type)
                 : !c_type_is_pointer(output.type)) ||
          (addresses[i] = lower_asm_memory_output_address(p, lvalue)) < 0 ||
          !expect(p, ")"))
        return defer_unsupported_asm(p, open, assembly);
    }
    if (!expect(p, ":") || !parse_inline_asm_operand_name(p) ||
        !inline_asm_string_sequence_is(p, "D", &p->pos) || !expect(p, "("))
      return defer_unsupported_asm(p, open, assembly);
    c_value_t destination = parse_expr(p, 1);
    if (destination.value < 0 || !c_type_is_pointer(destination.type) ||
        !expect(p, ")") || !expect(p, ":") ||
        !inline_asm_string_sequence_is(p, "memory", &p->pos) ||
        !expect(p, ")") || !expect(p, ";"))
      return defer_unsupported_asm(p, open, assembly);
    free(assembly);
    return emit(p,
                (nyr_inst_t){.op = NYIR_INLINE_ASM,
                             .dst = -1,
                             .a = addresses[0],
                             .b = addresses[1],
                             .c = addresses[2],
                             .d = destination.value,
                             .imm = -183,
                             .symbol = "xsha256",
                             .effects = NYIR_EFFECT_READ_MEMORY |
                                        NYIR_EFFECT_WRITE_MEMORY |
                                        NYIR_EFFECT_CONTROL |
                                        NYIR_EFFECT_VOLATILE},
                NULL);
  }

  static const char xstore_template[] =
      ".byte 0x0F,0xA7,0xC0 /* xstore %%edi (addr=%0) */";
  if (!asm_goto && p->abi == NYC_ABI_SYSV_X86_64 &&
      inline_asm_template_is(assembly, assembly_len, xstore_template)) {
    static const char *const output_constraints[] = {"=a", "+d", "+D"};
    int output_values[3] = {-1, -1, -1};
    int output_addresses[3] = {-1, -1, -1};
    c_local_t *outputs[3] = {NULL, NULL, NULL};
    if (!expect(p, ":") || !parse_inline_asm_operand_name(p) ||
        !inline_asm_string_sequence_is(p, "=m", &p->pos) || !expect(p, "("))
      return defer_unsupported_asm(p, open, assembly);
    c_value_t memory_output = parse_expr(p, 1);
    const c_lvalue_t *memory_lvalue =
        memory_output.value >= 0 ? find_lvalue(p, memory_output.value) : NULL;
    if (!memory_lvalue || memory_lvalue->address < 0 ||
        memory_lvalue->type != C_U32 ||
        !discard_lvalue_read(p, memory_lvalue) || !expect(p, ")"))
      return defer_unsupported_asm(p, open, assembly);

    for (size_t i = 0; i < 3; ++i) {
      if (!expect(p, ",") || !parse_inline_asm_operand_name(p) ||
          !inline_asm_string_sequence_is(p, output_constraints[i], &p->pos) ||
          !expect(p, "("))
        return defer_unsupported_asm(p, open, assembly);
      c_value_t output = parse_expr(p, 1);
      const c_lvalue_t *lvalue =
          output.value >= 0 ? find_lvalue(p, output.value) : NULL;
      if (!lvalue || !lvalue->is_local || lvalue->local_slot < 0 ||
          !expect(p, ")"))
        return defer_unsupported_asm(p, open, assembly);
      outputs[i] = find_local_slot(p, (size_t)lvalue->local_slot);
      if (!outputs[i] || outputs[i]->is_const || outputs[i]->array_rank)
        return defer_unsupported_asm(p, open, assembly);
      if (i == 0) {
        if (outputs[i]->type != C_U32 || !discard_lvalue_read(p, lvalue))
          return defer_unsupported_asm(p, open, assembly);
      } else if ((i == 1 && (!c_type_is_integer(outputs[i]->type) ||
                             c_type_size(p->abi, outputs[i]->type) != 4)) ||
                 (i == 2 && !c_type_is_pointer(outputs[i]->type))) {
        return defer_unsupported_asm(p, open, assembly);
      }
      output_values[i] = output.value;
      output_addresses[i] =
          emit_value(p, (nyr_inst_t){.op = NYIR_ADDR_LOCAL,
                                     .dst = -1,
                                     .imm = lvalue->local_slot,
                                     .scalar_class = NYIR_SCALAR_I64_RAW});
      if (output_addresses[i] < 0)
        return defer_unsupported_asm(p, open, assembly);
    }
    if (!expect(p, ")") || !expect(p, ";"))
      return defer_unsupported_asm(p, open, assembly);
    free(assembly);
    return emit(p,
                (nyr_inst_t){.op = NYIR_INLINE_ASM,
                             .dst = -1,
                             .a = output_values[1],
                             .b = output_values[2],
                             .c = output_addresses[2],
                             .d = output_addresses[1],
                             .e = output_addresses[0],
                             .f = -1,
                             .imm = -178,
                             .symbol = "xstore",
                             .effects = NYIR_EFFECT_READ_MEMORY |
                                        NYIR_EFFECT_WRITE_MEMORY |
                                        NYIR_EFFECT_CONTROL |
                                        NYIR_EFFECT_VOLATILE},
                NULL);
  }

  static const char rep_movsl_tail_template[] = "rep movsl\n"
                                                "testb $2,%b4\n"
                                                "je 1f\n"
                                                "movsw\n"
                                                "1:\ttestb $1,%b4\n"
                                                "je 2f\n"
                                                "movsb\n"
                                                "2:";
  bool rep_movsl_test_tail =
      p->abi == NYC_ABI_SYSV_X86_64 &&
      inline_asm_template_is(assembly, assembly_len, rep_movsl_tail_template);
  bool rep_movsq_byte_tail =
      p->abi == NYC_ABI_SYSV_X86_64 &&
      inline_asm_template_is(assembly, assembly_len,
                             "rep movsq movq %4,%%rcx rep movsb");
  bool rep_movsl_byte_tail =
      p->abi == NYC_ABI_I386 &&
      inline_asm_template_is(assembly, assembly_len,
                             "rep movsl movl %4,%%ecx rep movsb");
  if (!asm_goto &&
      (rep_movsl_test_tail || rep_movsq_byte_tail || rep_movsl_byte_tail)) {
    int output_addresses[3] = {-1, -1, -1};
    c_local_t *outputs[3] = {NULL, NULL, NULL};
    static const char *const output_constraints[] = {"=&c", "=&D", "=&S"};
    for (size_t i = 0; i < 3; ++i) {
      if ((i == 0 && !expect(p, ":")) || (i && !expect(p, ",")) ||
          !parse_inline_asm_operand_name(p) ||
          !inline_asm_string_sequence_is(p, output_constraints[i], &p->pos) ||
          !expect(p, "("))
        return defer_unsupported_asm(p, open, assembly);
      c_value_t output = parse_expr(p, 1);
      const c_lvalue_t *lvalue =
          output.value >= 0 ? find_lvalue(p, output.value) : NULL;
      if (!lvalue || !lvalue->is_local || lvalue->local_slot < 0 ||
          !expect(p, ")"))
        return defer_unsupported_asm(p, open, assembly);
      outputs[i] = find_local_slot(p, (size_t)lvalue->local_slot);
      if (!outputs[i] || outputs[i]->is_const || outputs[i]->array_rank ||
          !c_type_is_integer(outputs[i]->type) ||
          c_type_size(p->abi, outputs[i]->type) !=
              c_type_size(p->abi, C_CHAR_PTR))
        return defer_unsupported_asm(p, open, assembly);
      if (i > 0) {
        output_addresses[i] = lower_asm_memory_output_address(p, lvalue);
        if (output_addresses[i] < 0)
          return defer_unsupported_asm(p, open, assembly);
      } else if (!discard_lvalue_read(p, lvalue))
        return defer_unsupported_asm(p, open, assembly);
    }

    const char *const input_constraints[] = {
        "0", rep_movsl_test_tail ? "q" : "g", "1", "2"};
    int inputs[4] = {-1, -1, -1, -1};
    for (size_t i = 0; i < 4; ++i) {
      if ((i == 0 && !expect(p, ":")) || (i && !expect(p, ",")) ||
          !inline_asm_string_sequence_is(p, input_constraints[i], &p->pos) ||
          !expect(p, "("))
        return defer_unsupported_asm(p, open, assembly);
      c_value_t input = parse_expr(p, 1);
      bool valid_type = i < 2 ? c_type_is_integer(input.type)
                              : c_type_is_integer(input.type) ||
                                    c_type_is_pointer(input.type);
      if (input.value < 0 || !valid_type || !expect(p, ")"))
        return defer_unsupported_asm(p, open, assembly);
      inputs[i] = input.value;
    }
    if (!expect(p, ":") ||
        !inline_asm_string_sequence_is(p, "memory", &p->pos) ||
        !expect(p, ")") || !expect(p, ";"))
      return defer_unsupported_asm(p, open, assembly);

    free(assembly);
    if (!emit(p,
              (nyr_inst_t){
                  .op = NYIR_INLINE_ASM,
                  .dst = -1,
                  .a = inputs[2],
                  .b = inputs[3],
                  .c = inputs[0],
                  .d = inputs[1],
                  .e = output_addresses[1],
                  .f = output_addresses[2],
                  .imm = rep_movsl_test_tail ? -177 : -191,
                  .symbol = rep_movsl_test_tail   ? "rep_movsl_tail"
                            : rep_movsq_byte_tail ? "rep_movsq_byte_tail"
                                                  : "rep_movsl_byte_tail",
                  .effects = NYIR_EFFECT_READ_MEMORY |
                             NYIR_EFFECT_WRITE_MEMORY | NYIR_EFFECT_CONTROL |
                             NYIR_EFFECT_VOLATILE},
              NULL))
      return false;
    int zero = emit_i64_constant(p, 0);
    return zero >= 0 && lower_local_store(p, outputs[0], zero);
  }

  static const char *const repeat_io[] = {"rep outsb", "rep outsw", "rep outsl",
                                          "rep insb",  "rep insw",  "rep insl"};
  for (size_t form = 0; !asm_goto && p->abi == NYC_ABI_SYSV_X86_64 &&
                        form < sizeof(repeat_io) / sizeof(*repeat_io);
       ++form) {
    if (!inline_asm_template_is(assembly, assembly_len, repeat_io[form]))
      continue;
    int addresses[2] = {-1, -1};
    const char *constraints[] = {form < 3 ? "+S" : "+D", "+c"};
    for (size_t i = 0; i < 2; ++i) {
      if ((i == 0 && !expect(p, ":")) || (i && !expect(p, ",")) ||
          !parse_inline_asm_operand_name(p) ||
          !inline_asm_string_sequence_is(p, constraints[i], &p->pos) ||
          !expect(p, "("))
        return defer_unsupported_asm(p, open, assembly);
      c_value_t output = parse_expr(p, 1);
      const c_lvalue_t *lvalue =
          output.value >= 0 ? find_lvalue(p, output.value) : NULL;
      if (!lvalue || c_type_size(p->abi, output.type) != 8 ||
          (i ? !c_type_is_integer(output.type)
             : !c_type_is_pointer(output.type)) ||
          (addresses[i] = lower_asm_memory_output_address(p, lvalue)) < 0 ||
          !expect(p, ")"))
        return defer_unsupported_asm(p, open, assembly);
    }
    if (!expect(p, ":") || !parse_inline_asm_operand_name(p) ||
        !inline_asm_string_sequence_is(p, "d", &p->pos) || !expect(p, "("))
      return defer_unsupported_asm(p, open, assembly);
    c_value_t port = parse_expr(p, 1);
    if (port.value < 0 || !c_type_is_integer(port.type) || !expect(p, ")") ||
        !expect(p, ":") ||
        !inline_asm_string_sequence_is(p, "memory", &p->pos) ||
        !expect(p, ")") || !expect(p, ";"))
      return defer_unsupported_asm(p, open, assembly);
    free(assembly);
    return emit(p,
                (nyr_inst_t){.op = NYIR_INLINE_ASM,
                             .dst = -1,
                             .a = addresses[0],
                             .b = addresses[1],
                             .c = port.value,
                             .imm = -186,
                             .symbol = repeat_io[form],
                             .effects = NYIR_EFFECT_READ_MEMORY |
                                        NYIR_EFFECT_WRITE_MEMORY |
                                        NYIR_EFFECT_IO | NYIR_EFFECT_VOLATILE},
                NULL);
  }

  static const char *const single_movs[] = {"movsb", "movsw", "movsl", "movsq"};
  for (size_t form = 0; !asm_goto && p->abi == NYC_ABI_SYSV_X86_64 &&
                        form < sizeof(single_movs) / sizeof(*single_movs);
       ++form) {
    if (!inline_asm_template_is(assembly, assembly_len, single_movs[form]))
      continue;
    int addresses[2] = {-1, -1}, values[2] = {-1, -1};
    static const char *const outputs[] = {"=&D", "=&S"};
    static const char *const inputs[] = {"0", "1"};
    for (size_t i = 0; i < 2; ++i) {
      if ((i == 0 && !expect(p, ":")) || (i && !expect(p, ",")) ||
          !parse_inline_asm_operand_name(p) ||
          !inline_asm_string_sequence_is(p, outputs[i], &p->pos) ||
          !expect(p, "("))
        return defer_unsupported_asm(p, open, assembly);
      c_value_t output = parse_expr(p, 1);
      const c_lvalue_t *lvalue =
          output.value >= 0 ? find_lvalue(p, output.value) : NULL;
      if (!lvalue || c_type_size(p->abi, output.type) != 8 ||
          (!c_type_is_pointer(output.type) &&
           !c_type_is_integer(output.type)) ||
          (addresses[i] = lower_asm_memory_output_address(p, lvalue)) < 0 ||
          !expect(p, ")"))
        return defer_unsupported_asm(p, open, assembly);
    }
    for (size_t i = 0; i < 2; ++i) {
      if ((i == 0 && !expect(p, ":")) || (i && !expect(p, ",")) ||
          !parse_inline_asm_operand_name(p) ||
          !inline_asm_string_sequence_is(p, inputs[i], &p->pos) ||
          !expect(p, "("))
        return defer_unsupported_asm(p, open, assembly);
      c_value_t input = parse_expr(p, 1);
      if (input.value < 0 || c_type_size(p->abi, input.type) != 8 ||
          (!c_type_is_pointer(input.type) && !c_type_is_integer(input.type)) ||
          !expect(p, ")"))
        return defer_unsupported_asm(p, open, assembly);
      values[i] = input.value;
    }
    if (!expect(p, ":") ||
        !inline_asm_string_sequence_is(p, "memory", &p->pos) ||
        !expect(p, ")") || !expect(p, ";"))
      return defer_unsupported_asm(p, open, assembly);
    free(assembly);
    return emit(p,
                (nyr_inst_t){.op = NYIR_INLINE_ASM,
                             .dst = -1,
                             .a = values[0],
                             .b = values[1],
                             .c = addresses[0],
                             .d = addresses[1],
                             .imm = -182,
                             .symbol = single_movs[form],
                             .effects = NYIR_EFFECT_READ_MEMORY |
                                        NYIR_EFFECT_WRITE_MEMORY |
                                        NYIR_EFFECT_CONTROL |
                                        NYIR_EFFECT_VOLATILE},
                NULL);
  }

  if (!asm_goto && assembly_len == sizeof("rep movsb") - 1 &&
      memcmp(assembly, "rep movsb", sizeof("rep movsb") - 1) == 0) {
    int values[3] = {-1, -1, -1};
    int addresses[3] = {-1, -1, -1};
    static const char *const constraints[] = {"+D", "+S", "+c"};
    for (size_t i = 0; i < 3; ++i) {
      if ((i == 0 && !expect(p, ":")) || (i && !expect(p, ",")) ||
          !parse_inline_asm_operand_name(p) ||
          !inline_asm_string_sequence_is(p, constraints[i], &p->pos) ||
          !expect(p, "(")) {
        free(assembly);
        return false;
      }
      c_value_t output = parse_expr(p, 1);
      const c_lvalue_t *lvalue =
          output.value >= 0 ? find_lvalue(p, output.value) : NULL;
      if (output.value < 0 || !lvalue ||
          (i < 2 ? !c_type_is_pointer(output.type)
                 : !c_type_is_integer(output.type)) ||
          !expect(p, ")")) {
        free(assembly);
        return false;
      }
      int address = lvalue->address;
      if (address < 0 && lvalue->is_local)
        address =
            emit_value(p, (nyr_inst_t){.op = NYIR_ADDR_LOCAL,
                                       .dst = -1,
                                       .imm = lvalue->local_slot,
                                       .scalar_class = NYIR_SCALAR_I64_RAW});
      if (address < 0) {
        free(assembly);
        return false;
      }
      values[i] = output.value;
      addresses[i] = address;
    }
    if (!expect(p, ":") || !expect(p, ":") ||
        !inline_asm_string_sequence_is(p, "memory", &p->pos) ||
        !expect(p, ")") || !expect(p, ";")) {
      free(assembly);
      return false;
    }
    free(assembly);
    return emit(p,
                (nyr_inst_t){.op = NYIR_INLINE_ASM,
                             .dst = -1,
                             .a = values[0],
                             .b = values[1],
                             .c = values[2],
                             .d = addresses[0],
                             .e = addresses[1],
                             .f = addresses[2],
                             .imm = -135,
                             .symbol = "rep_movsb",
                             .effects = NYIR_EFFECT_READ_MEMORY |
                                        NYIR_EFFECT_WRITE_MEMORY |
                                        NYIR_EFFECT_CONTROL |
                                        NYIR_EFFECT_VOLATILE},
                NULL);
  }

  if (!asm_goto && p->abi == NYC_ABI_I386 &&
      inline_asm_template_is(assembly, assembly_len,
                             "pushw %%es ; movw %2,%%es ; shrw %%cx ; jnc 1f ; "
                             "stosw 1: rep stosl ; popw %%es")) {
    static const char *const constraints[] = {"+D", "+c"};
    int addresses[2], values[2], inputs[2];
    for (size_t i = 0; i < 2; ++i) {
      ny_ctok_t name;
      if ((i == 0 && !expect(p, ":")) || (i && !expect(p, ",")) ||
          !inline_asm_string_sequence_is(p, constraints[i], &p->pos) ||
          !expect(p, "(") || !parse_name(p, &name) || !expect(p, ")"))
        return defer_unsupported_asm(p, open, assembly);
      c_local_t *local = find_local(p, name);
      if (!local || local->is_const || local->array_rank ||
          !c_type_is_integer(local->type) ||
          c_type_size(p->abi, local->type) != 4)
        return defer_unsupported_asm(p, open, assembly);
      local->has_integer_constant = false;
      local->address_taken = true;
      addresses[i] =
          emit_value(p, (nyr_inst_t){.op = NYIR_ADDR_LOCAL,
                                     .dst = -1,
                                     .imm = (int64_t)local->slot,
                                     .scalar_class = NYIR_SCALAR_I64_RAW});
      values[i] = lower_local_load(p, local);
      if (addresses[i] < 0 || values[i] < 0)
        return defer_unsupported_asm(p, open, assembly);
    }
    static const char *const input_constraints[] = {"bdS", "a"};
    for (size_t i = 0; i < 2; ++i) {
      if ((i == 0 && !expect(p, ":")) || (i && !expect(p, ",")) ||
          !inline_asm_string_sequence_is(p, input_constraints[i], &p->pos) ||
          !expect(p, "("))
        return defer_unsupported_asm(p, open, assembly);
      c_value_t input = parse_expr(p, 1);
      if (input.value < 0 || !c_type_is_integer(input.type) ||
          c_type_size(p->abi, input.type) != (i ? 4 : 2) || !expect(p, ")"))
        return defer_unsupported_asm(p, open, assembly);
      inputs[i] = input.value;
    }
    if (!expect(p, ")") || !expect(p, ";"))
      return defer_unsupported_asm(p, open, assembly);
    free(assembly);
    return emit(p,
                (nyr_inst_t){
                    .op = NYIR_INLINE_ASM,
                    .dst = -1,
                    .a = values[0],
                    .b = values[1],
                    .c = inputs[0],
                    .d = inputs[1],
                    .e = addresses[0],
                    .f = addresses[1],
                    .imm = -195,
                    .symbol = "es_stosw_stosl",
                    .effects = NYIR_EFFECT_READ_MEMORY |
                               NYIR_EFFECT_WRITE_MEMORY | NYIR_EFFECT_MAY_TRAP |
                               NYIR_EFFECT_CONTROL | NYIR_EFFECT_VOLATILE},
                NULL);
  }

  static const char *const repeat_stos_templates[] = {"rep stosb", "rep stosw",
                                                      "rep stosl", "rep stosq"};
  static const char *const repeat_stos_symbols[] = {"rep_stosb", "rep_stosw",
                                                    "rep_stosl", "rep_stosq"};
  c_parser_t early_stosb_probe = *p;
  if (tok_is(&early_stosb_probe, early_stosb_probe.pos, ":"))
    ++early_stosb_probe.pos;
  size_t early_stosb_constraint_end = early_stosb_probe.pos;
  bool early_stosb_constraints = inline_asm_string_sequence_is(
      &early_stosb_probe, "=&c", &early_stosb_constraint_end);
  const char *early_stos_symbol = NULL;
  if (!asm_goto && tok_is(p, p->pos, ":") && early_stosb_constraints) {
    for (size_t i = 0;
         i < sizeof(repeat_stos_templates) / sizeof(repeat_stos_templates[0]);
         ++i) {
      if (assembly_len == strlen(repeat_stos_templates[i]) &&
          memcmp(assembly, repeat_stos_templates[i], assembly_len) == 0) {
        early_stos_symbol = repeat_stos_symbols[i];
        break;
      }
    }
  }
  if (early_stos_symbol) {
    int output_addresses[2] = {-1, -1};
    static const char *const output_constraints[] = {"=&c", "=&D"};
    for (size_t i = 0; i < 2; ++i) {
      if ((i == 0 && !expect(p, ":")) || (i && !expect(p, ",")) ||
          !parse_inline_asm_operand_name(p) ||
          !inline_asm_string_sequence_is(p, output_constraints[i], &p->pos) ||
          !expect(p, "(")) {
        free(assembly);
        return false;
      }
      c_value_t output = parse_expr(p, 1);
      const c_lvalue_t *lvalue =
          output.value >= 0 ? find_lvalue(p, output.value) : NULL;
      if (output.value < 0 || !lvalue || !c_type_is_integer(output.type) ||
          !expect(p, ")")) {
        free(assembly);
        return false;
      }
      output_addresses[i] = lvalue->address;
      if (output_addresses[i] < 0 && lvalue->is_local)
        output_addresses[i] =
            emit_value(p, (nyr_inst_t){.op = NYIR_ADDR_LOCAL,
                                       .dst = -1,
                                       .imm = lvalue->local_slot,
                                       .scalar_class = NYIR_SCALAR_I64_RAW});
      if (output_addresses[i] < 0) {
        free(assembly);
        return false;
      }
    }
    int byte_value = -1, pointer_value = -1, count_value = -1;
    static const char *const input_constraints[] = {"a", "1", "0"};
    for (size_t i = 0; i < 3; ++i) {
      if ((i == 0 && !expect(p, ":")) || (i && !expect(p, ",")) ||
          !parse_inline_asm_operand_name(p) ||
          !inline_asm_string_sequence_is(p, input_constraints[i], &p->pos) ||
          !expect(p, "(")) {
        free(assembly);
        return false;
      }
      c_value_t input = parse_expr(p, 1);
      if (input.value < 0 || !expect(p, ")") ||
          (i == 0 && !c_type_is_integer(input.type)) ||
          (i == 1 && !c_type_is_pointer(input.type)) ||
          (i == 2 && !c_type_is_integer(input.type))) {
        free(assembly);
        return false;
      }
      if (i == 0)
        byte_value = input.value;
      else if (i == 1)
        pointer_value = input.value;
      else
        count_value = input.value;
    }
    if (!expect(p, ":") ||
        !inline_asm_string_sequence_is(p, "memory", &p->pos) ||
        !expect(p, ")") || !expect(p, ";")) {
      free(assembly);
      return false;
    }
    free(assembly);
    return emit(p,
                (nyr_inst_t){.op = NYIR_INLINE_ASM,
                             .dst = -1,
                             .a = pointer_value,
                             .b = count_value,
                             .c = byte_value,
                             .d = output_addresses[1],
                             .e = output_addresses[0],
                             .f = -1,
                             .imm = -136,
                             .symbol = early_stos_symbol,
                             .effects = NYIR_EFFECT_READ_MEMORY |
                                        NYIR_EFFECT_WRITE_MEMORY |
                                        NYIR_EFFECT_CONTROL |
                                        NYIR_EFFECT_VOLATILE},
                NULL);
  }

  const char *repeat_stos_symbol = NULL;
  if (!asm_goto) {
    for (size_t i = 0;
         i < sizeof(repeat_stos_templates) / sizeof(repeat_stos_templates[0]);
         ++i) {
      if (assembly_len == strlen(repeat_stos_templates[i]) &&
          memcmp(assembly, repeat_stos_templates[i], assembly_len) == 0) {
        repeat_stos_symbol = repeat_stos_symbols[i];
        break;
      }
    }
  }
  if (repeat_stos_symbol) {
    int values[3] = {-1, -1, -1};
    int addresses[2] = {-1, -1};
    static const char *const constraints[] = {"+D", "+c"};
    for (size_t i = 0; i < 2; ++i) {
      if ((i == 0 && !expect(p, ":")) || (i && !expect(p, ",")) ||
          !parse_inline_asm_operand_name(p) ||
          !inline_asm_string_sequence_is(p, constraints[i], &p->pos) ||
          !expect(p, "(")) {
        free(assembly);
        return false;
      }
      c_value_t output = parse_expr(p, 1);
      const c_lvalue_t *lvalue =
          output.value >= 0 ? find_lvalue(p, output.value) : NULL;
      if (output.value < 0 || !lvalue ||
          (i == 0 ? !c_type_is_pointer(output.type)
                  : !c_type_is_integer(output.type)) ||
          !expect(p, ")")) {
        free(assembly);
        return false;
      }
      int address = lvalue->address;
      if (address < 0 && lvalue->is_local)
        address =
            emit_value(p, (nyr_inst_t){.op = NYIR_ADDR_LOCAL,
                                       .dst = -1,
                                       .imm = lvalue->local_slot,
                                       .scalar_class = NYIR_SCALAR_I64_RAW});
      if (address < 0) {
        free(assembly);
        return false;
      }
      values[i] = output.value;
      addresses[i] = address;
    }
    if (!expect(p, ":") || !inline_asm_string_sequence_is(p, "a", &p->pos) ||
        !expect(p, "(")) {
      free(assembly);
      return false;
    }
    c_value_t fill = parse_expr(p, 1);
    if (fill.value < 0 || !c_type_is_integer(fill.type) || !expect(p, ")") ||
        !expect(p, ":") ||
        !inline_asm_string_sequence_is(p, "memory", &p->pos) ||
        !expect(p, ")") || !expect(p, ";")) {
      free(assembly);
      return false;
    }
    free(assembly);
    return emit(p,
                (nyr_inst_t){.op = NYIR_INLINE_ASM,
                             .dst = -1,
                             .a = values[0],
                             .b = values[1],
                             .c = fill.value,
                             .d = addresses[0],
                             .e = addresses[1],
                             .f = -1,
                             .imm = -136,
                             .symbol = repeat_stos_symbol,
                             .effects = NYIR_EFFECT_READ_MEMORY |
                                        NYIR_EFFECT_WRITE_MEMORY |
                                        NYIR_EFFECT_CONTROL |
                                        NYIR_EFFECT_VOLATILE},
                NULL);
  }

  if (!asm_goto &&
      inline_asm_template_is(assembly, assembly_len, "test %3,%3 repe cmpsb")) {
    if (p->abi == NYC_ABI_WIN64) {
      fail(p, "repeated CMPSB is not supported for the Windows ABI");
      free(assembly);
      return false;
    }
    ny_ctok_t condition_name;
    if (!expect(p, ":") ||
        !inline_asm_string_sequence_is(p, "=@ccnz", &p->pos) ||
        !expect(p, "(") || !parse_name(p, &condition_name) || !expect(p, ")")) {
      free(assembly);
      return false;
    }
    static const char *const constraints[] = {"+D", "+S", "+c"};
    int values[3], addresses[3];
    for (size_t i = 0; i < 3; ++i) {
      ny_ctok_t name;
      if (!expect(p, ",") || !parse_inline_asm_operand_name(p) ||
          !inline_asm_string_sequence_is(p, constraints[i], &p->pos) ||
          !expect(p, "(") || !parse_name(p, &name) || !expect(p, ")")) {
        free(assembly);
        return false;
      }
      c_local_t *local = find_local(p, name);
      if (!local || local->is_const || local->array_rank ||
          (i < 2 ? !c_type_is_pointer(local->type)
                 : (!c_type_is_integer(local->type) ||
                    c_type_size(p->abi, local->type) !=
                        c_type_size(p->abi, C_CHAR_PTR)))) {
        fail(p, "repeated CMPSB requires tied pointer and count locals");
        free(assembly);
        return false;
      }
      local->has_integer_constant = false;
      local->address_taken = true;
      addresses[i] =
          emit_value(p, (nyr_inst_t){.op = NYIR_ADDR_LOCAL,
                                     .dst = -1,
                                     .imm = (int64_t)local->slot,
                                     .scalar_class = NYIR_SCALAR_I64_RAW});
      values[i] = lower_local_load(p, local);
      if (i == 2 && c_type_size(p->abi, local->type) <= 4)
        values[i] = normalize_u32(p, values[i]);
      if (addresses[i] < 0 || values[i] < 0) {
        free(assembly);
        return false;
      }
    }
    bool clobbers = false;
    if (accept(p, ":"))
      clobbers = accept(p, ":");
    c_local_t *condition = find_local(p, condition_name);
    if (!condition || condition->is_const || condition->array_rank ||
        !c_type_is_integer(condition->type) ||
        (clobbers && !inline_asm_clobber_list_is(p, &p->pos)) ||
        !expect(p, ")") || !expect(p, ";")) {
      free(assembly);
      return false;
    }
    free(assembly);
    int result = emit_value(
        p, (nyr_inst_t){.op = NYIR_INLINE_ASM,
                        .dst = -1,
                        .a = values[0],
                        .b = values[1],
                        .c = values[2],
                        .d = addresses[0],
                        .e = addresses[1],
                        .f = addresses[2],
                        .imm = -189,
                        .symbol = "repe_cmpsb",
                        .scalar_class = NYIR_SCALAR_I64_RAW,
                        .effects = NYIR_EFFECT_READ_MEMORY |
                                   NYIR_EFFECT_WRITE_MEMORY |
                                   NYIR_EFFECT_CONTROL | NYIR_EFFECT_VOLATILE});
    return result >= 0 && lower_local_store(p, condition, result);
  }

  if (!asm_goto && p->abi != NYC_ABI_I386 &&
      inline_asm_template_is(assembly, assembly_len, "mov %[address],%%r9")) {
    ny_ctok_t operand;
    if (!expect(p, ":") || !expect(p, ":") ||
        !parse_named_inline_asm_operand(p, &operand) ||
        !inline_asm_name_matches(operand, (const unsigned char *)"address",
                                 7) ||
        !inline_asm_string_sequence_is(p, "g", &p->pos) || !expect(p, "(")) {
      free(assembly);
      return false;
    }
    c_value_t input = parse_expr(p, 1);
    if (input.value < 0 || !c_type_is_integer(input.type) ||
        c_type_size(p->abi, input.type) != 8 || !expect(p, ")") ||
        (accept(p, ":") && !inline_asm_clobber_list_is(p, &p->pos)) ||
        !expect(p, ")") || !expect(p, ";")) {
      free(assembly);
      return false;
    }
    free(assembly);
    return emit(p,
                (nyr_inst_t){.op = NYIR_INLINE_ASM,
                             .dst = -1,
                             .a = input.value,
                             .imm = -192,
                             .symbol = "mov_r9",
                             .effects = NYIR_EFFECT_READ_MEMORY |
                                        NYIR_EFFECT_WRITE_MEMORY |
                                        NYIR_EFFECT_VOLATILE},
                NULL);
  }

  bool raw_rdmsr =
      !asm_goto && inline_asm_template_is(assembly, assembly_len, "rdmsr");
  bool raw_wrmsr =
      !asm_goto && inline_asm_template_is(assembly, assembly_len, "wrmsr");
  if (raw_rdmsr || raw_wrmsr) {
    int operands[3] = {-1, -1, -1};
    if (!expect(p, ":")) {
      free(assembly);
      return false;
    }
    if (raw_rdmsr) {
      static const char *const outputs[] = {"=a", "=d"};
      for (size_t i = 0; i < 2; ++i) {
        if ((i && !expect(p, ",")) || !parse_inline_asm_operand_name(p) ||
            !inline_asm_string_sequence_is(p, outputs[i], &p->pos) ||
            !expect(p, "(")) {
          free(assembly);
          return false;
        }
        c_value_t output = parse_expr(p, 1);
        if (!c_type_is_integer(output.type) ||
            c_type_size(p->abi, output.type) != 4) {
          fail(p, "raw RDMSR outputs require 32-bit integer lvalues");
          free(assembly);
          return false;
        }
        const c_lvalue_t *lvalue =
            output.value >= 0 ? find_lvalue(p, output.value) : NULL;
        operands[i + 1] = lower_asm_memory_output_address(p, lvalue);
        if (operands[i + 1] < 0 || !expect(p, ")")) {
          free(assembly);
          return false;
        }
      }
    }
    if (!expect(p, ":")) {
      free(assembly);
      return false;
    }
    size_t input_count = raw_rdmsr ? 1 : 3;
    for (size_t i = 0; i < input_count; ++i) {
      if ((i && !expect(p, ",")) || !parse_inline_asm_operand_name(p)) {
        free(assembly);
        return false;
      }
      static const char *const constraints[] = {"c", "a", "d"};
      size_t reg = 0;
      for (; reg < input_count; ++reg)
        if (inline_asm_string_sequence_is(p, constraints[reg], &p->pos))
          break;
      if (reg == input_count || operands[reg] >= 0 || !expect(p, "(")) {
        free(assembly);
        return false;
      }
      c_value_t value = parse_expr(p, 1);
      if (value.value < 0 || !c_type_is_integer(value.type) ||
          !expect(p, ")")) {
        free(assembly);
        return false;
      }
      operands[reg] = value.value;
    }
    if ((accept(p, ":") && !inline_asm_clobber_list_is(p, &p->pos)) ||
        !expect(p, ")") || !expect(p, ";")) {
      free(assembly);
      return false;
    }
    free(assembly);
    return emit(
        p,
        (nyr_inst_t){.op = NYIR_INLINE_ASM,
                     .dst = -1,
                     .a = operands[0],
                     .b = operands[1],
                     .c = operands[2],
                     .d = -1,
                     .e = -1,
                     .f = -1,
                     .imm = raw_rdmsr ? -187 : -188,
                     .symbol = raw_rdmsr ? "rdmsr" : "wrmsr",
                     .effects = NYIR_EFFECT_CONTROL | NYIR_EFFECT_VOLATILE |
                                (raw_rdmsr ? NYIR_EFFECT_WRITE_MEMORY : 0)},
        NULL);
  }

  if (!asm_goto && assembly_len == sizeof("cpuid") - 1 &&
      memcmp(assembly, "cpuid", sizeof("cpuid") - 1) == 0) {
    int output_addresses[4] = {-1, -1, -1, -1};
    bool output_seen[4] = {false, false, false, false};
    unsigned output_registers[4] = {0};
    int inputs[2] = {-1, -1};
    bool input_seen[2] = {false, false};
    static const char *const output_constraints[] = {"=a", "=b", "=c", "=d"};
    static const char *const tied_constraints[] = {"+a", "+b", "+c", "+d"};
    if (!expect(p, ":")) {
      free(assembly);
      return false;
    }
    size_t output_count = 0;
    while (!tok_is(p, p->pos, ":") && !tok_is(p, p->pos, ")")) {
      if ((output_count && !expect(p, ",")) ||
          !parse_inline_asm_operand_name(p)) {
        fail(p, "invalid CPUID output operand list");
        free(assembly);
        return false;
      }
      size_t reg = 0;
      bool read_write = false;
      size_t constraint_end = p->pos;
      for (; reg < 4; ++reg) {
        size_t candidate_end = p->pos;
        if (inline_asm_string_sequence_is(p, output_constraints[reg],
                                          &candidate_end)) {
          constraint_end = candidate_end;
          break;
        }
        if (inline_asm_string_sequence_is(p, tied_constraints[reg],
                                          &candidate_end)) {
          constraint_end = candidate_end;
          read_write = true;
          break;
        }
      }
      if (reg == 4 || output_seen[reg]) {
        fail(p, "unsupported CPUID output register constraint");
        free(assembly);
        return false;
      }
      p->pos = constraint_end;
      if (!expect(p, "(")) {
        free(assembly);
        return false;
      }
      c_value_t output = parse_expr(p, 1);
      const c_lvalue_t *lvalue =
          output.value >= 0 ? find_lvalue(p, output.value) : NULL;
      if (output.value < 0) {
        fail(p, "CPUID output is not a value");
        free(assembly);
        return false;
      }
      if (!c_type_is_integer(output.type)) {
        fail(p, "CPUID output requires an integer type");
        free(assembly);
        return false;
      }
      if (!lvalue) {
        fail(p, "CPUID output requires an addressable lvalue");
        free(assembly);
        return false;
      }
      int output_address = read_write
                               ? lvalue->address
                               : lower_asm_memory_output_address(p, lvalue);
      if (read_write && lvalue->is_local) {
        c_local_t *local = find_local_slot(p, lvalue->local_slot);
        if (!local || local->is_const) {
          free(assembly);
          return false;
        }
        local->has_integer_constant = false;
        local->address_taken = true;
      }
      if (output_address < 0 && lvalue->is_local)
        output_address =
            emit_value(p, (nyr_inst_t){.op = NYIR_ADDR_LOCAL,
                                       .dst = -1,
                                       .imm = lvalue->local_slot,
                                       .scalar_class = NYIR_SCALAR_I64_RAW});
      if (output_address < 0) {
        fail(p, "CPUID outputs require integer lvalues");
        free(assembly);
        return false;
      }
      if (!expect(p, ")")) {
        fail(p, "CPUID output operand is missing its closing parenthesis");
        free(assembly);
        return false;
      }
      output_seen[reg] = true;
      output_addresses[reg] = output_address;
      output_registers[output_count] = (unsigned)reg;
      if (read_write && (reg == 0 || reg == 2)) {
        unsigned input = reg == 0 ? 0 : 1;
        inputs[input] = output.value;
        input_seen[input] = true;
      }
      ++output_count;
      if (!tok_is(p, p->pos, ",") && !tok_is(p, p->pos, ":") &&
          !tok_is(p, p->pos, ")")) {
        fail(p, "invalid CPUID output separator");
        free(assembly);
        return false;
      }
    }
    if (!output_count) {
      free(assembly);
      return false;
    }
    bool has_inputs = accept(p, ":");
    size_t input_count = 0;
    while (has_inputs && !tok_is(p, p->pos, ":") && !tok_is(p, p->pos, ")")) {
      if ((input_count && !expect(p, ",")) ||
          !parse_inline_asm_operand_name(p)) {
        free(assembly);
        return false;
      }
      static const char *const input_constraints[] = {"a", "c", "0",
                                                      "1", "2", "3"};
      int input_register = -1;
      size_t constraint_end = p->pos;
      for (size_t i = 0;
           i < sizeof(input_constraints) / sizeof(input_constraints[0]); ++i) {
        size_t candidate_end = p->pos;
        if (!inline_asm_string_sequence_is(p, input_constraints[i],
                                           &candidate_end))
          continue;
        if (i < 2)
          input_register = (int)i;
        else if (i - 2 < output_count) {
          unsigned reg = output_registers[i - 2];
          input_register = reg == 0 ? 0 : reg == 2 ? 1 : -1;
        }
        constraint_end = candidate_end;
        break;
      }
      if (input_register < 0 || input_seen[input_register]) {
        fail(p, "unsupported CPUID input register constraint");
        free(assembly);
        return false;
      }
      p->pos = constraint_end;
      if (!expect(p, "(")) {
        free(assembly);
        return false;
      }
      c_value_t input = parse_expr(p, 1);
      if (input.value < 0 || !c_type_is_integer(input.type) ||
          !expect(p, ")")) {
        fail(p, "CPUID inputs require integer expressions");
        free(assembly);
        return false;
      }
      inputs[input_register] = input.value;
      input_seen[input_register] = true;
      ++input_count;
      if (!tok_is(p, p->pos, ",") && !tok_is(p, p->pos, ":") &&
          !tok_is(p, p->pos, ")")) {
        fail(p, "invalid CPUID input separator");
        free(assembly);
        return false;
      }
    }
    bool has_clobbers = accept(p, ":");
    if (!input_seen[0])
      inputs[0] = emit_i64_constant(p, 0);
    if (!input_seen[1])
      inputs[1] = emit_i64_constant(p, 0);
    for (size_t reg = 0; reg < 4; ++reg) {
      if (output_seen[reg])
        continue;
      output_addresses[reg] =
          emit_value(p, (nyr_inst_t){.op = NYIR_ALLOCA,
                                     .dst = -1,
                                     .imm = 4,
                                     .scalar_class = NYIR_SCALAR_I64_RAW});
    }
    if (inputs[0] < 0 || inputs[1] < 0 || output_addresses[0] < 0 ||
        output_addresses[1] < 0 || output_addresses[2] < 0 ||
        output_addresses[3] < 0 ||
        (has_clobbers && !inline_asm_clobber_list_is(p, &p->pos)) ||
        !expect(p, ")") || !expect(p, ";")) {
      fail(p, "invalid CPUID clobber list or statement terminator");
      free(assembly);
      return false;
    }
    free(assembly);
    return emit(p,
                (nyr_inst_t){.op = NYIR_INLINE_ASM,
                             .dst = -1,
                             .a = inputs[0],
                             .b = inputs[1],
                             .c = output_addresses[0],
                             .d = output_addresses[1],
                             .e = output_addresses[2],
                             .f = output_addresses[3],
                             .imm = -31,
                             .symbol = "cpuid",
                             .effects = NYIR_EFFECT_READ_MEMORY |
                                        NYIR_EFFECT_WRITE_MEMORY |
                                        NYIR_EFFECT_CONTROL |
                                        NYIR_EFFECT_VOLATILE},
                NULL);
  }

  static const char *const control_register_reads[] = {
      "mov %%cr0,%0", "mov %%cr1,%0", "mov %%cr2,%0", "mov %%cr3,%0",
      "mov %%cr4,%0"};
  static const char *const control_register_names[] = {"cr0", "cr1", "cr2",
                                                       "cr3", "cr4"};
  for (size_t i = 0;
       i < sizeof(control_register_reads) / sizeof(control_register_reads[0]);
       ++i) {
    if (!inline_asm_template_is(assembly, assembly_len,
                                control_register_reads[i]))
      continue;
    ny_ctok_t output_name;
    if (!expect(p, ":") || !parse_inline_asm_operand_name(p) ||
        !inline_asm_string_sequence_is(p, "=r", &p->pos) || !expect(p, "(") ||
        !parse_name(p, &output_name) || !expect(p, ")")) {
      free(assembly);
      return false;
    }
    bool has_clobbers = false;
    if (accept(p, ":"))
      has_clobbers = accept(p, ":");
    c_local_t *output = find_local(p, output_name);
    if (!output || !c_type_is_integer(output->type) || output->is_const ||
        output->array_rank ||
        (has_clobbers && !inline_asm_clobber_list_is(p, &p->pos)) ||
        !expect(p, ")") || !expect(p, ";")) {
      free(assembly);
      return false;
    }
    free(assembly);
    int result = emit_value(p, (nyr_inst_t){.op = NYIR_INLINE_ASM,
                                            .dst = -1,
                                            .a = -1,
                                            .b = -1,
                                            .c = -1,
                                            .d = -1,
                                            .e = -1,
                                            .f = -1,
                                            .imm = -65 - (int64_t)i,
                                            .symbol = control_register_names[i],
                                            .scalar_class = NYIR_SCALAR_I64_RAW,
                                            .effects = NYIR_EFFECT_VOLATILE});
    if (result >= 0)
      result = normalize_integer(p, output->type, result);
    return result >= 0 && lower_local_store(p, output, result);
  }

  static const char *const control_register_writes[] = {
      "mov %0,%%cr0", "mov %0,%%cr1", "mov %0,%%cr2", "mov %0,%%cr3",
      "mov %0,%%cr4"};
  for (size_t i = 0;
       i < sizeof(control_register_writes) / sizeof(control_register_writes[0]);
       ++i) {
    if (assembly_len != strlen(control_register_writes[i]) ||
        memcmp(assembly, control_register_writes[i], assembly_len) != 0)
      continue;
    if (!expect(p, ":")) {
      free(assembly);
      return false;
    }
    bool read_write_output = inline_asm_string_sequence_is(p, "+r", &p->pos);
    if ((!read_write_output &&
         (!expect(p, ":") ||
          !inline_asm_string_sequence_is(p, "r", &p->pos))) ||
        !expect(p, "(")) {
      free(assembly);
      return false;
    }
    c_value_t value = parse_expr(p, 1);
    if (value.value < 0 || !c_type_is_integer(value.type) || !expect(p, ")")) {
      free(assembly);
      return false;
    }
    bool has_clobbers = false;
    if (read_write_output) {
      if (accept(p, ":"))
        has_clobbers = accept(p, ":");
    } else {
      has_clobbers = accept(p, ":");
    }
    if ((has_clobbers && !inline_asm_clobber_list_is(p, &p->pos)) ||
        !expect(p, ")") || !expect(p, ";")) {
      free(assembly);
      return false;
    }
    free(assembly);
    return emit(
        p,
        (nyr_inst_t){.op = NYIR_INLINE_ASM,
                     .dst = -1,
                     .a = value.value,
                     .b = -1,
                     .c = -1,
                     .d = -1,
                     .e = -1,
                     .f = -1,
                     .imm = -60 - (int64_t)i,
                     .symbol = control_register_names[i],
                     .effects = NYIR_EFFECT_CONTROL | NYIR_EFFECT_VOLATILE},
        NULL);
  }

  static const struct {
    const char *assembly;
    const char *symbol;
    int64_t lowering;
  } task_register_loads[] = {{"ltr %w0", "ltr", -75},
                             {"lldt %w0", "lldt", -76}};
  for (size_t i = 0;
       i < sizeof(task_register_loads) / sizeof(task_register_loads[0]); ++i) {
    if (assembly_len != strlen(task_register_loads[i].assembly) ||
        memcmp(assembly, task_register_loads[i].assembly, assembly_len) != 0)
      continue;
    if (!expect(p, ":") || !expect(p, ":") ||
        !inline_asm_string_sequence_is(p, "q", &p->pos) || !expect(p, "(")) {
      free(assembly);
      return false;
    }
    c_value_t value = parse_expr(p, 1);
    if (value.value < 0 || !c_type_is_integer(value.type) || !expect(p, ")") ||
        !expect(p, ")") || !expect(p, ";")) {
      free(assembly);
      return false;
    }
    free(assembly);
    return emit(
        p,
        (nyr_inst_t){.op = NYIR_INLINE_ASM,
                     .dst = -1,
                     .a = value.value,
                     .b = -1,
                     .c = -1,
                     .d = -1,
                     .e = -1,
                     .f = -1,
                     .imm = task_register_loads[i].lowering,
                     .symbol = task_register_loads[i].symbol,
                     .effects = NYIR_EFFECT_CONTROL | NYIR_EFFECT_VOLATILE},
        NULL);
  }

  if (!asm_goto && p->abi == NYC_ABI_I386 &&
      (inline_asm_template_is(assembly, assembly_len, "lcallw *%0") ||
       inline_asm_template_is(assembly, assembly_len, "lcallw *%P1"))) {
    if (!expect(p, ":") || !expect(p, ":") ||
        !parse_inline_asm_operand_name(p) ||
        !inline_asm_string_sequence_is(p, "m", &p->pos) || !expect(p, "("))
      return defer_unsupported_asm(p, open, assembly);
    c_value_t input = parse_expr(p, 1);
    const c_lvalue_t *memory =
        input.value >= 0 ? find_asm_memory_lvalue(p, input.value) : NULL;
    int address = memory ? memory->address : -1;
    if (address < 0 && input.type == C_AGG)
      address = input.value;
    if (address < 0 || !expect(p, ")") || !expect(p, ":"))
      return defer_unsupported_asm(p, open, assembly);
    static const char *const clobbers[] = {"eax", "ebx", "ecx", "edx"};
    for (size_t i = 0; i < 4; ++i)
      if ((i && !expect(p, ",")) ||
          !inline_asm_string_sequence_is(p, clobbers[i], &p->pos))
        return defer_unsupported_asm(p, open, assembly);
    if (!expect(p, ")") || !expect(p, ";"))
      return defer_unsupported_asm(p, open, assembly);
    free(assembly);
    return emit(p,
                (nyr_inst_t){
                    .op = NYIR_INLINE_ASM,
                    .dst = -1,
                    .a = address,
                    .imm = -194,
                    .symbol = "lcallw",
                    .effects = NYIR_EFFECT_CALL | NYIR_EFFECT_READ_MEMORY |
                               NYIR_EFFECT_WRITE_MEMORY | NYIR_EFFECT_MAY_TRAP |
                               NYIR_EFFECT_CONTROL | NYIR_EFFECT_VOLATILE},
                NULL);
  }

  static const struct {
    const char *assembly;
    const char *segment;
  } plain_segment_loads[] = {{"movw %0,%%ds", "ds"}, {"movw %0,%%es", "es"},
                             {"movw %0,%%ss", "ss"}, {"movw %0,%%fs", "fs"},
                             {"movw %0,%%gs", "gs"}, {"mov %0,%%ds", "ds"},
                             {"mov %0,%%es", "es"},  {"mov %0,%%ss", "ss"},
                             {"mov %0,%%fs", "fs"},  {"mov %0,%%gs", "gs"}};
  for (size_t i = 0; !asm_goto && i < sizeof(plain_segment_loads) /
                                          sizeof(plain_segment_loads[0]);
       ++i) {
    if (!inline_asm_template_is(assembly, assembly_len,
                                plain_segment_loads[i].assembly))
      continue;
    if (!expect(p, ":") || !expect(p, ":") ||
        !inline_asm_string_sequence_is(p, "rm", &p->pos) || !expect(p, "(")) {
      free(assembly);
      return false;
    }
    c_value_t value = parse_expr(p, 1);
    if (value.value < 0 || !c_type_is_integer(value.type) || !expect(p, ")") ||
        !expect(p, ")") || !expect(p, ";")) {
      free(assembly);
      return false;
    }
    free(assembly);
    return emit(
        p,
        (nyr_inst_t){.op = NYIR_INLINE_ASM,
                     .dst = -1,
                     .a = value.value,
                     .b = -1,
                     .c = -1,
                     .d = -1,
                     .e = -1,
                     .f = -1,
                     .imm = -79,
                     .symbol = plain_segment_loads[i].segment,
                     .effects = NYIR_EFFECT_CONTROL | NYIR_EFFECT_VOLATILE},
        NULL);
  }

  if (assembly_len &&
      memmem(assembly, assembly_len, "movw %0, %%fs",
             sizeof("movw %0, %%fs") - 1) &&
      memmem(assembly, assembly_len, ".long 5", sizeof(".long 5") - 1) &&
      memmem(assembly, assembly_len, ".pushsection __ex_table",
             sizeof(".pushsection __ex_table") - 1)) {
    if (!expect(p, ":") || !expect(p, ":") ||
        !inline_asm_string_sequence_is(p, "rm", &p->pos) || !expect(p, "(")) {
      free(assembly);
      return false;
    }
    c_value_t value = parse_expr(p, 1);
    if (value.value < 0 || !c_type_is_integer(value.type) || !expect(p, ")") ||
        !expect(p, ":") || !inline_asm_clobber_list_is(p, &p->pos) ||
        !expect(p, ")") || !expect(p, ";")) {
      free(assembly);
      return false;
    }
    free(assembly);
    return emit(
        p,
        (nyr_inst_t){.op = NYIR_INLINE_ASM,
                     .dst = -1,
                     .a = value.value,
                     .b = -1,
                     .c = -1,
                     .d = -1,
                     .e = -1,
                     .f = -1,
                     .imm = -70,
                     .symbol = "load_fs_clear",
                     .effects = NYIR_EFFECT_CONTROL | NYIR_EFFECT_VOLATILE},
        NULL);
  }

  static const char *const segment_read_templates[] = {
      "movw %%ds,%0",  "movw %%es,%0",  "movw %%ss,%0",  "movw %%fs,%0",
      "movw %%gs,%0",  "movl %%ds,%k0", "movl %%es,%k0", "movl %%ss,%k0",
      "movl %%fs,%k0", "movl %%gs,%k0"};
  static const char *const segment_read_names[] = {
      "ds", "es", "ss", "fs", "gs", "ds", "es", "ss", "fs", "gs"};
  for (size_t i = 0;
       i < sizeof(segment_read_templates) / sizeof(segment_read_templates[0]);
       ++i) {
    if (assembly_len != strlen(segment_read_templates[i]) ||
        memcmp(assembly, segment_read_templates[i], assembly_len) != 0)
      continue;
    ny_ctok_t output_name;
    const char *constraint = i < 5 ? "=rm" : "=r";
    if (!expect(p, ":") ||
        !inline_asm_string_sequence_is(p, constraint, &p->pos) ||
        !expect(p, "(") || !parse_name(p, &output_name) || !expect(p, ")") ||
        !expect(p, ")") || !expect(p, ";")) {
      free(assembly);
      return false;
    }
    c_local_t *output = find_local(p, output_name);
    if (!output || output->array_rank || output->type != C_U16 ||
        output->is_const) {
      free(assembly);
      return false;
    }
    free(assembly);
    int result = emit_value(
        p, (nyr_inst_t){.op = NYIR_INLINE_ASM,
                        .dst = -1,
                        .a = -1,
                        .b = -1,
                        .c = -1,
                        .d = -1,
                        .e = -1,
                        .f = -1,
                        .imm = -80,
                        .symbol = segment_read_names[i],
                        .effects = NYIR_EFFECT_CONTROL | NYIR_EFFECT_VOLATILE});
    return result >= 0 && lower_local_store(p, output, result);
  }

  static const char *const segment_register_outputs[] = {"ss", "cs"};
  for (size_t i = 0;
       i < sizeof(segment_register_outputs) / sizeof(*segment_register_outputs);
       ++i) {
    char instruction[32];
    int instruction_len =
        snprintf(instruction, sizeof(instruction), "mov %%%%%s,%%k0",
                 segment_register_outputs[i]);
    if (instruction_len < 0 || assembly_len != (size_t)instruction_len ||
        memcmp(assembly, instruction, assembly_len) != 0)
      continue;
    if (!expect(p, ":") || !inline_asm_string_sequence_is(p, "=a", &p->pos) ||
        !expect(p, "(")) {
      free(assembly);
      return false;
    }
    c_value_t destination = parse_expr(p, 1);
    const c_lvalue_t *target =
        destination.value >= 0 ? find_lvalue(p, destination.value) : NULL;
    if (destination.value < 0 || !target || target->bitfield ||
        !c_type_is_integer(target->type) || target->address < 0 ||
        !expect(p, ")") || !expect(p, ")") || !expect(p, ";")) {
      free(assembly);
      return false;
    }
    free(assembly);
    int selector = emit_value(
        p, (nyr_inst_t){.op = NYIR_INLINE_ASM,
                        .dst = -1,
                        .a = -1,
                        .b = -1,
                        .c = -1,
                        .d = -1,
                        .e = -1,
                        .f = -1,
                        .imm = -80,
                        .symbol = segment_register_outputs[i],
                        .effects = NYIR_EFFECT_CONTROL | NYIR_EFFECT_VOLATILE});
    if (selector < 0)
      return false;
    bool stored = target->type == C_I16 || target->type == C_U16
                      ? lower_store_u16_field(p, target->address, selector,
                                              target->type, target->is_volatile)
                  : target->type == C_I32 || target->type == C_U32
                      ? lower_store_i32_field(p, target->address, selector,
                                              target->is_volatile)
                      : lower_store_field(p, target->address, selector, false,
                                          target->is_volatile);
    return stored;
  }

  static const char *const checked_segment_loads[] = {"ss", "ds", "es"};
  for (size_t i = 0;
       i < sizeof(checked_segment_loads) / sizeof(checked_segment_loads[0]);
       ++i) {
    char instruction[40];
    int instruction_len =
        snprintf(instruction, sizeof(instruction), "movl %%k0,%%%%%s",
                 checked_segment_loads[i]);
    if (instruction_len < 0 ||
        !memmem(assembly, assembly_len, instruction, (size_t)instruction_len) ||
        !memmem(assembly, assembly_len, ".macro extable_type_reg",
                sizeof(".macro extable_type_reg") - 1))
      continue;
    if (!expect(p, ":") || !inline_asm_string_sequence_is(p, "+r", &p->pos) ||
        !expect(p, "(")) {
      free(assembly);
      return false;
    }
    c_value_t value = parse_expr(p, 1);
    const c_lvalue_t *lvalue =
        value.value >= 0 ? find_lvalue(p, value.value) : NULL;
    c_local_t *output = lvalue && lvalue->is_local
                            ? find_local_slot(p, (size_t)lvalue->local_slot)
                            : NULL;
    if (value.value < 0 || !c_type_is_integer(value.type) || !lvalue ||
        !output || !expect(p, ")") || !expect(p, ":") || !expect(p, ":") ||
        !inline_asm_clobber_list_is(p, &p->pos) || !expect(p, ")") ||
        !expect(p, ";")) {
      free(assembly);
      return false;
    }
    free(assembly);
    (void)output;
    return emit(
        p,
        (nyr_inst_t){.op = NYIR_INLINE_ASM,
                     .dst = -1,
                     .a = value.value,
                     .b = -1,
                     .c = -1,
                     .d = -1,
                     .e = -1,
                     .f = -1,
                     .imm = -134,
                     .symbol = checked_segment_loads[i],
                     .effects = NYIR_EFFECT_CONTROL | NYIR_EFFECT_VOLATILE},
        NULL);
  }

  bool call_on_stack_template =
      !asm_goto && INLINE_ASM_HAS("movq\t%%rsp, (%[tos])") &&
      INLINE_ASM_HAS("movq\t%[tos], %%rsp") &&
      INLINE_ASM_HAS("call %c[__func]") && INLINE_ASM_HAS("popq\t%%rsp");
  if (call_on_stack_template) {
    bool enters_irq = INLINE_ASM_HAS("call irq_enter_rcu");
    bool exits_irq = INLINE_ASM_HAS("call irq_exit_rcu");
    if (enters_irq != exits_irq) {
      free(assembly);
      fail(p, "IRQ stack switch requires both IRQ boundary calls");
      return false;
    }
    bool irq_stack_switch = enters_irq;
    ny_ctok_t tos_output, stack_pointer_output;
    if (!expect(p, ":") || !inline_asm_string_sequence_is(p, "+r", &p->pos) ||
        !expect(p, "(") || !parse_name(p, &tos_output) || !expect(p, ")") ||
        !expect(p, ",") || !inline_asm_string_sequence_is(p, "+r", &p->pos) ||
        !expect(p, "(") || !parse_name(p, &stack_pointer_output) ||
        !expect(p, ")") || !expect(p, ":")) {
      free(assembly);
      return false;
    }
    c_local_t *tos_local = find_local(p, tos_output);
    ny_ctok_t function_operand, function_name;
    if (!tos_local || tos_local->array_rank ||
        !inline_asm_name_matches(stack_pointer_output,
                                 (const unsigned char *)"current_stack_pointer",
                                 sizeof("current_stack_pointer") - 1) ||
        !tos_local->fixed_register.len ||
        tos_local->fixed_register.len != sizeof("\"r11\"") - 1 ||
        memcmp(tos_local->fixed_register.start, "\"r11\"",
               sizeof("\"r11\"") - 1) != 0 ||
        tos_local->fixed_register_marker >= p->out->len ||
        !parse_named_inline_asm_operand(p, &function_operand) ||
        !inline_asm_name_matches(function_operand,
                                 (const unsigned char *)"__func", 6) ||
        !inline_asm_string_sequence_is(p, "i", &p->pos) || !expect(p, "(") ||
        !parse_name(p, &function_name) || !expect(p, ")") || !expect(p, ",")) {
      free(assembly);
      return false;
    }
    p->out->data[tos_local->fixed_register_marker] =
        (nyr_inst_t){.op = NYIR_NOP, .dst = -1, .a = -1, .b = -1};
    tos_local->fixed_register = (ny_ctok_t){0};
    ny_ctok_t tos_input_name;
    if (!parse_named_inline_asm_operand(p, &tos_input_name) ||
        !inline_asm_name_matches(tos_input_name, (const unsigned char *)"tos",
                                 3) ||
        !inline_asm_string_sequence_is(p, "r", &p->pos) || !expect(p, "(")) {
      free(assembly);
      return false;
    }
    c_value_t tos_value = parse_expr(p, 1);
    if (tos_value.value < 0 || !expect(p, ")")) {
      free(assembly);
      return false;
    }
    int argument_values[3] = {-1, -1, -1};
    size_t argument_count = 0;
    while (accept(p, ",")) {
      ny_ctok_t argument_name;
      size_t max_arguments = irq_stack_switch ? 2 : 3;
      if (argument_count == max_arguments ||
          !parse_named_inline_asm_operand(p, &argument_name) ||
          !inline_asm_string_sequence_is(p, "r", &p->pos) || !expect(p, "(")) {
        free(assembly);
        return false;
      }
      static const char *const argument_names[] = {"arg1", "arg2", "arg3"};
      if (!inline_asm_name_matches(
              argument_name,
              (const unsigned char *)argument_names[argument_count],
              strlen(argument_names[argument_count]))) {
        free(assembly);
        return false;
      }
      c_value_t argument = parse_expr(p, 1);
      if (argument.value < 0 || !expect(p, ")")) {
        free(assembly);
        return false;
      }
      argument_values[argument_count++] = argument.value;
    }
    size_t clobber_end = p->pos;
    if (!expect(p, ":") || !inline_asm_clobber_list_is(p, &clobber_end)) {
      free(assembly);
      return false;
    }
    p->pos = clobber_end;
    if (!expect(p, ")") || !expect(p, ";") ||
        (irq_stack_switch && argument_count == 0) || !function_name.start ||
        function_name.len == 0 || function_name.len >= 256 ||
        (!c_type_is_pointer(tos_value.type) &&
         !c_type_is_integer(tos_value.type))) {
      free(assembly);
      return false;
    }
    char target_buffer[256];
    memcpy(target_buffer, function_name.start, function_name.len);
    target_buffer[function_name.len] = '\0';
    const char *target = nyr_func_own_symbol_copy(p->out, target_buffer);
    free(assembly);
    if (!target)
      return false;
    return emit(
        p,
        (nyr_inst_t){.op = NYIR_INLINE_ASM,
                     .dst = -1,
                     .a = tos_value.value,
                     .b = argument_count ? argument_values[0] : -1,
                     .c = argument_count > 1 ? argument_values[1] : -1,
                     .d = argument_count > 2 ? argument_values[2] : -1,
                     .e = -1,
                     .f = -1,
                     .imm = irq_stack_switch ? -111 : -153,
                     .symbol = target,
                     .effects = NYIR_EFFECT_CALL | NYIR_EFFECT_READ_MEMORY |
                                NYIR_EFFECT_WRITE_MEMORY | NYIR_EFFECT_CONTROL |
                                NYIR_EFFECT_VOLATILE},
        NULL);
  }

  static const struct {
    const char *assembly;
    const char *constraint;
    const char *symbol;
    c_type_t type;
  } segment_reads[] = {{"movb %%fs:%1,%0", "=q", "fs8", C_U8},
                       {"movw %%fs:%1,%0", "=r", "fs16", C_U16},
                       {"movl %%fs:%1,%0", "=r", "fs32", C_U32},
                       {"movb %%gs:%1,%0", "=q", "gs8", C_U8},
                       {"movw %%gs:%1,%0", "=r", "gs16", C_U16},
                       {"movl %%gs:%1,%0", "=r", "gs32", C_U32}};
  for (size_t i = 0;
       !asm_goto && i < sizeof(segment_reads) / sizeof(segment_reads[0]); ++i) {
    if (!inline_asm_template_is(assembly, assembly_len,
                                segment_reads[i].assembly))
      continue;
    ny_ctok_t output_name;
    if (!expect(p, ":") ||
        !inline_asm_string_sequence_is(p, segment_reads[i].constraint,
                                       &p->pos) ||
        !expect(p, "(") || !parse_name(p, &output_name) || !expect(p, ")") ||
        !expect(p, ":") || !inline_asm_string_sequence_is(p, "m", &p->pos) ||
        !expect(p, "(")) {
      free(assembly);
      return false;
    }
    c_value_t input = parse_expr(p, 1);
    const c_lvalue_t *lvalue =
        input.value >= 0 ? find_lvalue(p, input.value) : NULL;
    c_local_t *output = find_local(p, output_name);
    int address = lvalue ? lvalue->address : -1;
    if (address < 0 || !discard_lvalue_read(p, lvalue) || !expect(p, ")") ||
        !expect(p, ")") || !expect(p, ";") || !output || output->array_rank ||
        output->type != segment_reads[i].type || output->is_const) {
      free(assembly);
      return false;
    }
    free(assembly);
    int result = emit_value(p, (nyr_inst_t){.op = NYIR_INLINE_ASM,
                                            .dst = -1,
                                            .a = address,
                                            .b = -1,
                                            .c = -1,
                                            .d = -1,
                                            .e = -1,
                                            .f = -1,
                                            .imm = -108,
                                            .symbol = segment_reads[i].symbol,
                                            .effects = NYIR_EFFECT_READ_MEMORY |
                                                       NYIR_EFFECT_CONTROL |
                                                       NYIR_EFFECT_VOLATILE});
    return result >= 0 && lower_local_store(p, output, result);
  }

  static const struct {
    const char *assembly;
    const char *constraint;
    const char *symbol;
  } segment_stores[] = {
      {"movb %1,%%fs:%0", "qi", "fs8"},  {"movw %1,%%fs:%0", "ri", "fs16"},
      {"movl %1,%%fs:%0", "ri", "fs32"}, {"movb %1,%%gs:%0", "qi", "gs8"},
      {"movw %1,%%gs:%0", "ri", "gs16"}, {"movl %1,%%gs:%0", "ri", "gs32"}};
  for (size_t i = 0;
       !asm_goto && i < sizeof(segment_stores) / sizeof(segment_stores[0]);
       ++i) {
    if (!inline_asm_template_is(assembly, assembly_len,
                                segment_stores[i].assembly))
      continue;
    if (!expect(p, ":") || !inline_asm_string_sequence_is(p, "+m", &p->pos) ||
        !expect(p, "("))
      return defer_unsupported_asm(p, open, assembly);
    c_value_t output = parse_expr(p, 1);
    const c_lvalue_t *lvalue =
        output.value >= 0 ? find_lvalue(p, output.value) : NULL;
    int address = lower_asm_memory_output_address(p, lvalue);
    if (address < 0 || !expect(p, ")") || !expect(p, ":") ||
        !inline_asm_string_sequence_is(p, segment_stores[i].constraint,
                                       &p->pos) ||
        !expect(p, "("))
      return defer_unsupported_asm(p, open, assembly);
    c_value_t input = parse_expr(p, 1);
    if (input.value < 0 || !c_type_is_integer(input.type) || !expect(p, ")") ||
        !expect(p, ")") || !expect(p, ";"))
      return defer_unsupported_asm(p, open, assembly);
    free(assembly);
    return emit(p,
                (nyr_inst_t){.op = NYIR_INLINE_ASM,
                             .dst = -1,
                             .a = address,
                             .b = input.value,
                             .imm = -193,
                             .symbol = segment_stores[i].symbol,
                             .effects = NYIR_EFFECT_WRITE_MEMORY |
                                        NYIR_EFFECT_VOLATILE},
                NULL);
  }

  const char *software_hweight =
      memmem(assembly, assembly_len, "call __sw_hweight64",
             sizeof("call __sw_hweight64") - 1)
          ? "__sw_hweight64"
      : memmem(assembly, assembly_len, "call __sw_hweight32",
               sizeof("call __sw_hweight32") - 1)
          ? "__sw_hweight32"
          : NULL;
  if (!asm_goto && software_hweight &&
      memmem(assembly, assembly_len, ".altinstructions",
             sizeof(".altinstructions") - 1)) {
    ny_ctok_t result_name, value_name;
    if (!expect(p, ":") || !parse_named_inline_asm_operand(p, &result_name) ||
        !inline_asm_name_matches(result_name, (const unsigned char *)"cnt",
                                 3) ||
        !inline_asm_string_sequence_is(p, "=a", &p->pos) || !expect(p, "(") ||
        !parse_name(p, &result_name) || !expect(p, ")") || !expect(p, ",") ||
        !parse_inline_asm_operand_name(p) ||
        !inline_asm_string_sequence_is(p, "+r", &p->pos) || !expect(p, "(")) {
      return defer_unsupported_asm(p, open, assembly);
    }
    c_value_t stack_pointer = parse_expr(p, 1);
    if (stack_pointer.value < 0 || !expect(p, ")") || !expect(p, ":") ||
        !parse_named_inline_asm_operand(p, &value_name) ||
        !inline_asm_name_matches(value_name, (const unsigned char *)"val", 3) ||
        !inline_asm_string_sequence_is(p, "D", &p->pos) || !expect(p, "(")) {
      return defer_unsupported_asm(p, open, assembly);
    }
    c_value_t value = parse_expr(p, 1);
    c_local_t *result = find_local(p, result_name);
    const c_type_t expected_value_type =
        strcmp(software_hweight, "__sw_hweight64") == 0 ? C_U64 : C_U32;
    const c_type_t expected_result_type =
        strcmp(software_hweight, "__sw_hweight64") == 0 ? C_U64 : C_U32;
    if (value.value < 0 || value.type != expected_value_type ||
        !expect(p, ")") || !expect(p, ")") || !expect(p, ";") || !result ||
        result->array_rank || result->type != expected_result_type) {
      return defer_unsupported_asm(p, open, assembly);
    }
    free(assembly);
    int result_value = emit_value(
        p, (nyr_inst_t){.op = NYIR_CALL,
                        .dst = -1,
                        .a = value.value,
                        .b = -1,
                        .c = -1,
                        .d = -1,
                        .e = -1,
                        .f = -1,
                        .imm = 1,
                        .flags = NYIR_INST_F_EXTERN,
                        .effects = NYIR_EFFECT_CALL | NYIR_EFFECT_VOLATILE,
                        .scalar_class = NYIR_SCALAR_I64_RAW,
                        .symbol = software_hweight});
    return result_value >= 0 && lower_local_store(p, result, result_value);
  }

  static const struct {
    const char *assembly;
    const char *output_constraint;
    int64_t lowering;
    c_type_t output_type;
  } port_input_templates[] = {
      {"inb %w1, %b0", "=a", -55, C_U8},
      {"inw %w1, %w0", "=a", -56, C_U16},
      {"inl %w1, %0", "=a", -57, C_U32},
  };
  for (size_t i = 0;
       i < sizeof(port_input_templates) / sizeof(port_input_templates[0]);
       ++i) {
    const size_t template_len = strlen(port_input_templates[i].assembly);
    if (assembly_len != template_len ||
        memcmp(assembly, port_input_templates[i].assembly, template_len) != 0)
      continue;
    ny_ctok_t output_name;
    size_t constraint_end = p->pos;
    if (!expect(p, ":") ||
        !inline_asm_string_sequence_is(
            p, port_input_templates[i].output_constraint, &constraint_end)) {
      free(assembly);
      return defer_unsupported_asm(p, open, NULL);
    }
    p->pos = constraint_end;
    if (!expect(p, "(") || !parse_name(p, &output_name) || !expect(p, ")") ||
        !expect(p, ":")) {
      free(assembly);
      return false;
    }
    constraint_end = p->pos;
    if (!inline_asm_string_sequence_is(p, "Nd", &constraint_end)) {
      free(assembly);
      return defer_unsupported_asm(p, open, NULL);
    }
    p->pos = constraint_end;
    if (!expect(p, "(")) {
      free(assembly);
      return false;
    }
    c_value_t port = parse_expr(p, 1);
    if (port.value < 0 || !c_type_is_integer(port.type) || !expect(p, ")")) {
      free(assembly);
      return false;
    }
    c_local_t *output = find_local(p, output_name);
    if (!output || output->array_rank ||
        output->type != port_input_templates[i].output_type ||
        !expect(p, ")") || !expect(p, ";")) {
      free(assembly);
      return defer_unsupported_asm(p, open, NULL);
    }
    free(assembly);
    int result = emit_value(
        p, (nyr_inst_t){.op = NYIR_INLINE_ASM,
                        .dst = -1,
                        .a = port.value,
                        .b = -1,
                        .c = -1,
                        .d = -1,
                        .e = -1,
                        .f = -1,
                        .imm = port_input_templates[i].lowering,
                        .symbol = port_input_templates[i].assembly,
                        .effects = NYIR_EFFECT_READ_MEMORY |
                                   NYIR_EFFECT_WRITE_MEMORY |
                                   NYIR_EFFECT_CONTROL | NYIR_EFFECT_VOLATILE});
    return result >= 0 && lower_local_store(p, output, result);
  }

  static const struct {
    const char *assembly;
    int64_t lowering;
    c_type_t value_type;
  } port_output_templates[] = {
      {"outb %b0, %w1", -52, C_U8},
      {"outw %w0, %w1", -53, C_U16},
      {"outl %0, %w1", -54, C_U32},
  };
  for (size_t i = 0;
       i < sizeof(port_output_templates) / sizeof(port_output_templates[0]);
       ++i) {
    const size_t template_len = strlen(port_output_templates[i].assembly);
    if (assembly_len != template_len ||
        memcmp(assembly, port_output_templates[i].assembly, template_len) != 0)
      continue;
    size_t constraint_end = p->pos;
    if (!expect(p, ":") || !expect(p, ":") ||
        !inline_asm_string_sequence_is(p, "a", &constraint_end)) {
      free(assembly);
      return defer_unsupported_asm(p, open, NULL);
    }
    p->pos = constraint_end;
    if (!expect(p, "(")) {
      free(assembly);
      return false;
    }
    c_value_t value = parse_expr(p, 1);
    if (value.value < 0 || value.type != port_output_templates[i].value_type ||
        !expect(p, ")") || !expect(p, ",")) {
      free(assembly);
      return false;
    }
    constraint_end = p->pos;
    if (!inline_asm_string_sequence_is(p, "Nd", &constraint_end)) {
      free(assembly);
      return defer_unsupported_asm(p, open, NULL);
    }
    p->pos = constraint_end;
    if (!expect(p, "(")) {
      free(assembly);
      return false;
    }
    c_value_t port = parse_expr(p, 1);
    if (port.value < 0 || !c_type_is_integer(port.type) || !expect(p, ")") ||
        !expect(p, ")") || !expect(p, ";")) {
      free(assembly);
      return false;
    }
    free(assembly);
    return emit(p,
                (nyr_inst_t){.op = NYIR_INLINE_ASM,
                             .dst = -1,
                             .a = value.value,
                             .b = -1,
                             .c = port.value,
                             .d = -1,
                             .e = -1,
                             .f = -1,
                             .imm = port_output_templates[i].lowering,
                             .symbol = port_output_templates[i].assembly,
                             .effects = NYIR_EFFECT_READ_MEMORY |
                                        NYIR_EFFECT_WRITE_MEMORY |
                                        NYIR_EFFECT_CONTROL |
                                        NYIR_EFFECT_VOLATILE},
                NULL);
  }

  bool wrmsr_alternative_template =
      INLINE_ASM_HAS("# ALT: oldinstr") &&
      INLINE_ASM_HAS("# ALT: replacement") && INLINE_ASM_HAS("wrmsr") &&
      INLINE_ASM_HAS("%c[feature]") && INLINE_ASM_HAS(".altinstructions");
  bool paravirt_void_call_template =
      !asm_goto && INLINE_ASM_HAS("# ALT: oldinstr") &&
      INLINE_ASM_HAS("call *%[paravirt_opptr]") &&
      INLINE_ASM_HAS(".altinstructions");
  bool clflush_memory_template =
      !asm_goto && assembly_len == sizeof("clflush %0") - 1 &&
      memcmp(assembly, "clflush %0", sizeof("clflush %0") - 1) == 0;
  const char *fxsave_mnemonic =
      assembly_len == sizeof("fxsaveq %[fx]") - 1 &&
              memcmp(assembly, "fxsaveq %[fx]", sizeof("fxsaveq %[fx]") - 1) ==
                  0
          ? "fxsave64"
      : assembly_len == sizeof("fxsave %[fx]") - 1 &&
              memcmp(assembly, "fxsave %[fx]", sizeof("fxsave %[fx]") - 1) == 0
          ? "fxsave"
      : assembly_len == sizeof("fxsaveq %0") - 1 &&
              memcmp(assembly, "fxsaveq %0", sizeof("fxsaveq %0") - 1) == 0
          ? "fxsave64"
      : assembly_len == sizeof("fxsave %0") - 1 &&
              memcmp(assembly, "fxsave %0", sizeof("fxsave %0") - 1) == 0
          ? "fxsave"
      : assembly_len == sizeof("fnsave %[fp]; fwait") - 1 &&
              memcmp(assembly, "fnsave %[fp]; fwait",
                     sizeof("fnsave %[fp]; fwait") - 1) == 0
          ? "fnsave"
          : NULL;
  bool fninit_status_template =
      assembly_len == sizeof("fninit ; fnstsw %0 ; fnstcw %1") - 1 &&
      memcmp(assembly, "fninit ; fnstsw %0 ; fnstcw %1",
             sizeof("fninit ; fnstsw %0 ; fnstcw %1") - 1) == 0;
  bool fdiv_bug_template = INLINE_ASM_HAS("fdivl %2") &&
                           INLINE_ASM_HAS("fistpl %0") &&
                           INLINE_ASM_HAS("fsubp %%st,%%st(1)");
  bool fildl_clear_template = INLINE_ASM_HAS("fnclex") &&
                              INLINE_ASM_HAS("emms") &&
                              INLINE_ASM_HAS("fildl %[addr]");
  bool ldmxcsr_template =
      assembly_len == sizeof("ldmxcsr %0") - 1 &&
      memcmp(assembly, "ldmxcsr %0", sizeof("ldmxcsr %0") - 1) == 0;
  bool fxrstor_extable_template =
      !asm_goto &&
      (INLINE_ASM_HAS("fxrstorq %[fx]") || INLINE_ASM_HAS("frstor %[fx]")) &&
      INLINE_ASM_HAS(".pushsection __ex_table");
  bool fxrstor_error_template =
      !asm_goto &&
      (INLINE_ASM_HAS("fxrstorq %[fx]") || INLINE_ASM_HAS("frstor %[fx]")) &&
      INLINE_ASM_HAS("extable_type_reg reg=%[err]");
  bool fxrstor_user_template =
      !asm_goto &&
      (INLINE_ASM_HAS("fxrstorq %[fx]") || INLINE_ASM_HAS("frstor %[fx]")) &&
      INLINE_ASM_HAS("clac") && INLINE_ASM_HAS(".long 15");
  bool put_user_nocheck_template =
      !asm_goto && INLINE_ASM_HAS("call __put_user_nocheck_%c[size]");
  bool put_user_template = !asm_goto &&
                           INLINE_ASM_HAS("call __put_user_%c[size]") &&
                           !put_user_nocheck_template;
  bool get_user_template =
      !asm_goto && (INLINE_ASM_HAS("call __get_user_%c[size]") ||
                    INLINE_ASM_HAS("call __get_user_nocheck_%c[size]"));
  if (get_user_template) {
    ny_ctok_t result_name, value_name, size_name;
    if (!expect(p, ":") || !parse_inline_asm_operand_name(p) ||
        !inline_asm_string_sequence_is(p, "=a", &p->pos) || !expect(p, "(") ||
        !parse_name(p, &result_name) || !expect(p, ")") || !expect(p, ",") ||
        !parse_inline_asm_operand_name(p) ||
        !inline_asm_string_sequence_is(p, "=r", &p->pos) || !expect(p, "(") ||
        !parse_name(p, &value_name) || !expect(p, ")") || !expect(p, ",") ||
        !parse_inline_asm_operand_name(p) ||
        !inline_asm_string_sequence_is(p, "+r", &p->pos) || !expect(p, "(")) {
      free(assembly);
      return false;
    }
    c_value_t stack_pointer = parse_expr(p, 1);
    if (stack_pointer.value < 0 || !expect(p, ")") || !expect(p, ":") ||
        !inline_asm_string_sequence_is(p, "0", &p->pos) || !expect(p, "(")) {
      free(assembly);
      return false;
    }
    c_value_t pointer = parse_expr(p, 1);
    if (pointer.value < 0 || !c_type_is_pointer(pointer.type) ||
        !expect(p, ")") || !expect(p, ",") ||
        !parse_named_inline_asm_operand(p, &size_name) ||
        !inline_asm_name_matches(size_name, (const unsigned char *)"size", 4) ||
        !inline_asm_string_sequence_is(p, "i", &p->pos) || !expect(p, "(")) {
      free(assembly);
      return false;
    }
    c_value_t size = parse_expr(p, 1);
    int64_t size_value = 0;
    c_local_t *result_local = find_local(p, result_name);
    c_local_t *value_local = find_local(p, value_name);
    if (size.value < 0 || !c_type_is_integer(size.type) || !expect(p, ")") ||
        !expect(p, ")") || !expect(p, ";") || !result_local || !value_local ||
        result_local == value_local || result_local->array_rank ||
        value_local->array_rank ||
        (result_local->type != C_I32 && result_local->type != C_I64) ||
        !c_type_is_integer(value_local->type) ||
        !c_ir_nonnegative_constant(p, size.value, &size_value) ||
        (size_value != 1 && size_value != 2 && size_value != 4 &&
         size_value != 8)) {
      free(assembly);
      return false;
    }
    if (value_local->fixed_register.len) {
      ny_ctok_t binding = value_local->fixed_register;
      const char *name = binding.start + 1;
      size_t length = binding.len - 2;
      if (length && *name == '%') {
        ++name;
        --length;
      }
      if (!((length == 3 &&
             (!memcmp(name, "rdx", 3) || !memcmp(name, "edx", 3))) ||
            (length == 2 && !memcmp(name, "dx", 2))) ||
          value_local->fixed_register_marker >= p->out->len) {
        free(assembly);
        fail(p, "get_user value output requires the DX register");
        return false;
      }
      p->out->data[value_local->fixed_register_marker] =
          (nyr_inst_t){.op = NYIR_NOP, .dst = -1, .a = -1, .b = -1};
      value_local->fixed_register = (ny_ctok_t){0};
    }
    char helper[48];
    char call_symbol[112];
    const char *helper_prefix =
        memmem(assembly, assembly_len, "call __get_user_nocheck_%c[size]",
               sizeof("call __get_user_nocheck_%c[size]") - 1)
            ? "call __get_user_nocheck_%lld"
            : "call __get_user_%lld";
    int helper_len =
        snprintf(helper, sizeof(helper), helper_prefix, (long long)size_value);
    int symbol_len =
        helper_len < 0 || (size_t)helper_len >= sizeof(helper)
            ? -1
            : snprintf(call_symbol, sizeof(call_symbol), "%s;nyc_slots=%zu,%zu",
                       helper, result_local->slot, value_local->slot);
    const char *lowered =
        symbol_len < 0 || (size_t)symbol_len >= sizeof(call_symbol)
            ? NULL
            : nyr_func_own_symbol_copy(p->out, call_symbol);
    int result_address =
        emit_value(p, (nyr_inst_t){.op = NYIR_ADDR_LOCAL,
                                   .dst = -1,
                                   .imm = (int64_t)result_local->slot});
    int value_address =
        emit_value(p, (nyr_inst_t){.op = NYIR_ADDR_LOCAL,
                                   .dst = -1,
                                   .imm = (int64_t)value_local->slot});
    free(assembly);
    if (!lowered || result_address < 0 || value_address < 0) {
      fail(p, "out of memory lowering get_user inline assembly");
      return false;
    }
    return emit(
        p,
        (nyr_inst_t){.op = NYIR_INLINE_ASM,
                     .dst = -1,
                     .a = pointer.value,
                     .b = -1,
                     .c = -1,
                     .d = result_address,
                     .e = value_address,
                     .f = -1,
                     .imm = -24,
                     .symbol = lowered,
                     .effects = NYIR_EFFECT_CALL | NYIR_EFFECT_READ_MEMORY |
                                NYIR_EFFECT_WRITE_MEMORY |
                                NYIR_EFFECT_VOLATILE | NYIR_EFFECT_MAY_TRAP},
        NULL);
  }
  if (put_user_template) {
    ny_ctok_t result_name, size_name;
    if (!expect(p, ":") || !parse_inline_asm_operand_name(p) ||
        !inline_asm_string_sequence_is(p, "=c", &p->pos) || !expect(p, "(") ||
        !parse_name(p, &result_name) || !expect(p, ")") || !expect(p, ",") ||
        !parse_inline_asm_operand_name(p) ||
        !inline_asm_string_sequence_is(p, "+r", &p->pos) || !expect(p, "(")) {
      free(assembly);
      return false;
    }
    c_value_t stack_pointer = parse_expr(p, 1);
    if (stack_pointer.value < 0 || !expect(p, ")") || !expect(p, ":") ||
        !inline_asm_string_sequence_is(p, "0", &p->pos) || !expect(p, "(")) {
      free(assembly);
      return false;
    }
    c_value_t pointer = parse_expr(p, 1);
    if (pointer.value < 0 || !c_type_is_pointer(pointer.type) ||
        !expect(p, ")") || !expect(p, ",") ||
        !parse_inline_asm_operand_name(p) ||
        !inline_asm_string_sequence_is(p, "r", &p->pos) || !expect(p, "(")) {
      free(assembly);
      return false;
    }
    c_value_t value = parse_expr(p, 1);
    if (value.value < 0 ||
        !(c_type_is_integer(value.type) || c_type_is_pointer(value.type)) ||
        !expect(p, ")") || !expect(p, ",") ||
        !parse_named_inline_asm_operand(p, &size_name) ||
        !inline_asm_name_matches(size_name, (const unsigned char *)"size", 4) ||
        !inline_asm_string_sequence_is(p, "i", &p->pos) || !expect(p, "(")) {
      free(assembly);
      return false;
    }
    c_value_t size = parse_expr(p, 1);
    int64_t size_value = 0;
    c_local_t *result_local = find_local(p, result_name);
    if (size.value < 0 || !c_type_is_integer(size.type) || !expect(p, ")") ||
        !expect(p, ":") || !inline_asm_clobber_list_is(p, &p->pos) ||
        !expect(p, ")") || !expect(p, ";") || !result_local ||
        result_local->type != C_I32 || result_local->array_rank ||
        result_local->is_const ||
        !c_ir_nonnegative_constant(p, size.value, &size_value) ||
        (size_value != 1 && size_value != 2 && size_value != 4 &&
         size_value != 8)) {
      free(assembly);
      return false;
    }
    char call_symbol[112];
    int symbol_len = snprintf(call_symbol, sizeof(call_symbol),
                              "call __put_user_%lld;nyc_slots=%zu",
                              (long long)size_value, result_local->slot);
    const char *lowered =
        symbol_len < 0 || (size_t)symbol_len >= sizeof(call_symbol)
            ? NULL
            : nyr_func_own_symbol_copy(p->out, call_symbol);
    int result_address =
        emit_value(p, (nyr_inst_t){.op = NYIR_ADDR_LOCAL,
                                   .dst = -1,
                                   .imm = (int64_t)result_local->slot});
    free(assembly);
    if (!lowered || result_address < 0) {
      fail(p, "out of memory lowering put_user inline assembly");
      return false;
    }
    return emit(
        p,
        (nyr_inst_t){.op = NYIR_INLINE_ASM,
                     .dst = -1,
                     .a = pointer.value,
                     .b = value.value,
                     .c = -1,
                     .d = result_address,
                     .e = -1,
                     .f = -1,
                     .imm = -34,
                     .symbol = lowered,
                     .effects = NYIR_EFFECT_CALL | NYIR_EFFECT_READ_MEMORY |
                                NYIR_EFFECT_WRITE_MEMORY |
                                NYIR_EFFECT_VOLATILE | NYIR_EFFECT_MAY_TRAP},
        NULL);
  }
  if (put_user_nocheck_template) {
    ny_ctok_t result_name, size_name;
    if (!expect(p, ":") || !parse_inline_asm_operand_name(p) ||
        !inline_asm_string_sequence_is(p, "=c", &p->pos) || !expect(p, "(") ||
        !parse_name(p, &result_name) || !expect(p, ")") || !expect(p, ",") ||
        !parse_inline_asm_operand_name(p) ||
        !inline_asm_string_sequence_is(p, "+r", &p->pos) || !expect(p, "(")) {
      fail(p, "invalid put_user_nocheck output or stack-pointer operand");
      free(assembly);
      return false;
    }
    c_value_t stack_pointer = parse_expr(p, 1);
    if (stack_pointer.value < 0 || !expect(p, ")") || !expect(p, ":") ||
        !inline_asm_string_sequence_is(p, "0", &p->pos) || !expect(p, "(")) {
      fail(p, "invalid put_user_nocheck tied pointer input");
      free(assembly);
      return false;
    }
    c_value_t pointer = parse_expr(p, 1);
    if (pointer.value < 0 || !c_type_is_pointer(pointer.type) ||
        !expect(p, ")") || !expect(p, ",") ||
        !parse_inline_asm_operand_name(p) ||
        !inline_asm_string_sequence_is(p, "r", &p->pos) || !expect(p, "(")) {
      fail(p, "invalid put_user_nocheck value input");
      free(assembly);
      return false;
    }
    c_value_t value = parse_expr(p, 1);
    if (value.value < 0 ||
        (!c_type_is_integer(value.type) && !c_type_is_pointer(value.type)) ||
        !expect(p, ")") || !expect(p, ",") ||
        !parse_named_inline_asm_operand(p, &size_name) ||
        !inline_asm_name_matches(size_name, (const unsigned char *)"size", 4) ||
        !inline_asm_string_sequence_is(p, "i", &p->pos) || !expect(p, "(")) {
      char message[160];
      snprintf(message, sizeof(message),
               "invalid put_user_nocheck size input at `%.*s` token %zu",
               (int)(current(p)->len > 32 ? 32 : current(p)->len),
               current(p)->start ? current(p)->start : "<eof>", p->pos);
      fail(p, message);
      free(assembly);
      return false;
    }
    c_value_t size = parse_expr(p, 1);
    c_local_t *result_local = find_local(p, result_name);
    if (size.value < 0 || !c_type_is_integer(size.type) || !expect(p, ")") ||
        !expect(p, ":")) {
      fail(p, "invalid put_user_nocheck immediate input");
      free(assembly);
      return false;
    }
    size_t clobber_end = p->pos;
    if (!inline_asm_clobber_list_is(p, &clobber_end)) {
      fail(p, "unsupported put_user_nocheck clobber list");
      free(assembly);
      return false;
    }
    p->pos = clobber_end;
    if (!expect(p, ")") || !expect(p, ";") || !result_local ||
        result_local->type != C_I32 || result_local->array_rank ||
        result_local->is_const) {
      fail(p, "invalid put_user_nocheck statement terminator or output");
      free(assembly);
      return false;
    }
    int result_address =
        emit_value(p, (nyr_inst_t){.op = NYIR_ADDR_LOCAL,
                                   .dst = -1,
                                   .imm = (int64_t)result_local->slot});
    free(assembly);
    return result_address >= 0 &&
           emit(p,
                (nyr_inst_t){.op = NYIR_INLINE_ASM,
                             .dst = -1,
                             .a = pointer.value,
                             .b = value.value,
                             .c = result_address,
                             .d = size.value,
                             .imm = -130,
                             .symbol = "put_user_nocheck",
                             .effects =
                                 NYIR_EFFECT_CALL | NYIR_EFFECT_READ_MEMORY |
                                 NYIR_EFFECT_WRITE_MEMORY |
                                 NYIR_EFFECT_VOLATILE | NYIR_EFFECT_MAY_TRAP},
                NULL);
  }

  bool fnsave_user_template =
      !asm_goto &&
      (INLINE_ASM_HAS("fnsave %[fx]") || INLINE_ASM_HAS("fxsaveq %[fx]") ||
       INLINE_ASM_HAS("fxsave %[fx]")) &&
      INLINE_ASM_HAS("clac") && INLINE_ASM_HAS(".long 15");
  if (fnsave_user_template) {
    bool fnsave = INLINE_ASM_HAS("fnsave %[fx]");
    ny_ctok_t result_name;
    if (!expect(p, ":") || !parse_inline_asm_operand_name(p) ||
        !inline_asm_string_sequence_is(p, "=a", &p->pos) || !expect(p, "(") ||
        !parse_name(p, &result_name) || !expect(p, ")") || !expect(p, ",") ||
        !parse_inline_asm_operand_name(p) ||
        !inline_asm_string_sequence_is(p, "=m", &p->pos) || !expect(p, "(")) {
      free(assembly);
      return false;
    }
    c_value_t output = parse_expr(p, 1);
    if (output.value < 0 || output.type != C_AGG || !expect(p, ")") ||
        !expect(p, ":") || !inline_asm_string_sequence_is(p, "0", &p->pos) ||
        !expect(p, "(")) {
      free(assembly);
      return false;
    }
    c_value_t zero = parse_expr(p, 1);
    if (zero.value < 0 || !expect(p, ")") || !expect(p, ",") ||
        !parse_inline_asm_operand_name(p) ||
        !inline_asm_string_sequence_is(p, "m", &p->pos) || !expect(p, "(")) {
      free(assembly);
      return false;
    }
    c_value_t input = parse_expr(p, 1);
    c_local_t *result_local = find_local(p, result_name);
    if (input.value < 0 || input.type != C_AGG || !expect(p, ")") ||
        !expect(p, ")") || !expect(p, ";") || !result_local ||
        result_local->type != C_I32 || result_local->array_rank ||
        result_local->is_const) {
      free(assembly);
      return false;
    }
    int address = output.value;
    free(assembly);
    int result = emit_value(
        p,
        (nyr_inst_t){.op = NYIR_INLINE_ASM,
                     .dst = -1,
                     .a = address,
                     .imm = -129,
                     .symbol = fnsave ? "fnsave_user" : "fxsave_user",
                     .effects = NYIR_EFFECT_WRITE_MEMORY | NYIR_EFFECT_CONTROL |
                                NYIR_EFFECT_VOLATILE | NYIR_EFFECT_MAY_TRAP});
    return result >= 0 && lower_local_store(p, result_local, result);
  }
  bool xgetbv_template = assembly_len == sizeof("xgetbv") - 1 &&
                         memcmp(assembly, "xgetbv", sizeof("xgetbv") - 1) == 0;
  bool xsave_alternative_template =
      !asm_goto && INLINE_ASM_HAS("xsave64 %[xa]") &&
      INLINE_ASM_HAS("extable_type_reg reg=%[err]");
  bool xsave_user_template =
      !asm_goto &&
      (INLINE_ASM_HAS("xsave64 %[xa]") || INLINE_ASM_HAS("xsaves64 %[xa]") ||
       INLINE_ASM_HAS("xrstor64 %[xa]") || INLINE_ASM_HAS("xrstors64 %[xa]")) &&
      INLINE_ASM_HAS(".long 15") && INLINE_ASM_HAS("xor %[err], %[err]");
  if (xsave_user_template) {
    ny_ctok_t result_name;
    if (!expect(p, ":") || !parse_inline_asm_operand_name(p) ||
        !inline_asm_string_sequence_is(p, "=a", &p->pos) || !expect(p, "(") ||
        !parse_name(p, &result_name) || !expect(p, ")") || !expect(p, ":") ||
        !parse_inline_asm_operand_name(p) ||
        !inline_asm_string_sequence_is(p, "m", &p->pos) || !expect(p, "(")) {
      free(assembly);
      return false;
    }
    c_value_t area = parse_expr(p, 1);
    if (area.value < 0 || area.type != C_AGG || !expect(p, ")") ||
        !expect(p, ",") || !parse_inline_asm_operand_name(p) ||
        !inline_asm_string_sequence_is(p, "a", &p->pos) || !expect(p, "(")) {
      free(assembly);
      return false;
    }
    c_value_t lmask = parse_expr(p, 1);
    if (lmask.value < 0 || !expect(p, ")") || !expect(p, ",") ||
        !parse_inline_asm_operand_name(p) ||
        !inline_asm_string_sequence_is(p, "d", &p->pos) || !expect(p, "(")) {
      free(assembly);
      return false;
    }
    c_value_t hmask = parse_expr(p, 1);
    c_local_t *result_local = find_local(p, result_name);
    size_t constraint_end = p->pos;
    if (hmask.value < 0 || !expect(p, ")") || !expect(p, ":") ||
        !inline_asm_clobber_list_is(p, &constraint_end) || !result_local ||
        result_local->type != C_I32 || result_local->array_rank ||
        result_local->is_const) {
      free(assembly);
      return false;
    }
    p->pos = constraint_end;
    if (!expect(p, ")") || !expect(p, ";")) {
      free(assembly);
      return false;
    }
    int result_address =
        emit_value(p, (nyr_inst_t){.op = NYIR_ADDR_LOCAL,
                                   .dst = -1,
                                   .imm = (int64_t)result_local->slot});
    const char *user_xstate_op =
        INLINE_ASM_HAS("xsaves64 %[xa]")    ? "xsaves_user"
        : INLINE_ASM_HAS("xrstors64 %[xa]") ? "xrstors_user"
        : INLINE_ASM_HAS("xrstor64 %[xa]")  ? "xrstor_user"
                                            : "xsave_user";
    free(assembly);
    int result =
        result_address < 0
            ? -1
            : emit_value(p, (nyr_inst_t){.op = NYIR_INLINE_ASM,
                                         .dst = -1,
                                         .a = area.value,
                                         .b = lmask.value,
                                         .c = hmask.value,
                                         .d = result_address,
                                         .imm = -131,
                                         .symbol = user_xstate_op,
                                         .effects = NYIR_EFFECT_WRITE_MEMORY |
                                                    NYIR_EFFECT_CONTROL |
                                                    NYIR_EFFECT_VOLATILE |
                                                    NYIR_EFFECT_MAY_TRAP});
    return result >= 0 && lower_local_store(p, result_local, result);
  }
  bool xrstor_alternative_template = !asm_goto &&
                                     INLINE_ASM_HAS("xrstor64 %[xa]") &&
                                     INLINE_ASM_HAS("__ex_table");
  if (xrstor_alternative_template) {
    if (!seek_inline_asm_constraint(p, "m") ||
        !inline_asm_string_sequence_is(p, "m", &p->pos) || !expect(p, "(")) {
      fail(p, "xrstor alternative memory operand");
      free(assembly);
      return false;
    }
    c_value_t area = parse_expr(p, 1);
    if (area.value < 0 || area.type != C_AGG || !expect(p, ")") ||
        !expect(p, ",") || !seek_inline_asm_constraint(p, "a") ||
        !inline_asm_string_sequence_is(p, "a", &p->pos) || !expect(p, "(")) {
      fail(p, "xrstor low mask operand");
      free(assembly);
      return false;
    }
    c_value_t lmask = parse_expr(p, 1);
    if (lmask.value < 0 || !expect(p, ")") || !expect(p, ",") ||
        !seek_inline_asm_constraint(p, "d") ||
        !inline_asm_string_sequence_is(p, "d", &p->pos) || !expect(p, "(")) {
      fail(p, "xrstor high mask operand");
      free(assembly);
      return false;
    }
    c_value_t hmask = parse_expr(p, 1);
    size_t clobber_end;
    if (hmask.value < 0 || !expect(p, ")") || !expect(p, ":") ||
        !inline_asm_clobber_list_is(p, &clobber_end)) {
      fail(p, "xrstor memory clobber");
      free(assembly);
      return false;
    }
    p->pos = clobber_end;
    if (!expect(p, ")") || !expect(p, ";")) {
      fail(p, "xrstor asm terminator");
      free(assembly);
      return false;
    }
    free(assembly);
    return emit(p,
                (nyr_inst_t){.op = NYIR_INLINE_ASM,
                             .dst = -1,
                             .a = area.value,
                             .b = lmask.value,
                             .c = hmask.value,
                             .imm = -127,
                             .symbol = "xrstor_alternative",
                             .effects = NYIR_EFFECT_READ_MEMORY |
                                        NYIR_EFFECT_CONTROL |
                                        NYIR_EFFECT_VOLATILE},
                NULL);
  }
  if (xsave_alternative_template) {
    ny_ctok_t result_name, xa_name;
    if (!expect(p, ":") || !parse_named_inline_asm_operand(p, &result_name) ||
        !inline_asm_name_matches(result_name, (const unsigned char *)"err",
                                 3) ||
        !inline_asm_string_sequence_is(p, "=r", &p->pos) || !expect(p, "(") ||
        !parse_name(p, &result_name) || !expect(p, ")") || !expect(p, ":") ||
        !parse_named_inline_asm_operand(p, &xa_name) ||
        !inline_asm_name_matches(xa_name, (const unsigned char *)"xa", 2) ||
        !inline_asm_string_sequence_is(p, "m", &p->pos) || !expect(p, "(")) {
      free(assembly);
      return false;
    }
    c_value_t area = parse_expr(p, 1);
    c_local_t *result_local = find_local(p, result_name);
    if (area.value < 0 || area.type != C_AGG || !expect(p, ")") ||
        !expect(p, ",") || !parse_inline_asm_operand_name(p) ||
        !inline_asm_string_sequence_is(p, "a", &p->pos) || !expect(p, "(")) {
      free(assembly);
      return false;
    }
    c_value_t lmask = parse_expr(p, 1);
    if (lmask.value < 0 || !expect(p, ")") || !expect(p, ",") ||
        !parse_inline_asm_operand_name(p) ||
        !inline_asm_string_sequence_is(p, "d", &p->pos) || !expect(p, "(")) {
      free(assembly);
      return false;
    }
    c_value_t hmask = parse_expr(p, 1);
    size_t clobber_end;
    if (hmask.value < 0 || !expect(p, ")") || !expect(p, ":") ||
        !inline_asm_clobber_list_is(p, &clobber_end)) {
      free(assembly);
      return false;
    }
    p->pos = clobber_end;
    if (!expect(p, ")") || !expect(p, ";") || !result_local ||
        result_local->type != C_I32 || result_local->array_rank ||
        result_local->is_const) {
      free(assembly);
      return false;
    }
    int result_address =
        emit_value(p, (nyr_inst_t){.op = NYIR_ADDR_LOCAL,
                                   .dst = -1,
                                   .imm = (int64_t)result_local->slot});
    free(assembly);
    return result_address >= 0 &&
           emit(p,
                (nyr_inst_t){.op = NYIR_INLINE_ASM,
                             .dst = -1,
                             .a = area.value,
                             .b = lmask.value,
                             .c = hmask.value,
                             .d = result_address,
                             .imm = -126,
                             .symbol = "xsave_alternative",
                             .effects = NYIR_EFFECT_WRITE_MEMORY |
                                        NYIR_EFFECT_CONTROL |
                                        NYIR_EFFECT_VOLATILE},
                NULL);
  }
  if (!asm_goto && xgetbv_template) {
    ny_ctok_t eax_name = {0}, edx_name = {0};
    bool operands_ok =
        expect(p, ":") && parse_inline_asm_operand_name(p) &&
        inline_asm_string_sequence_is(p, "=a", &p->pos) && expect(p, "(") &&
        parse_name(p, &eax_name) && expect(p, ")") && expect(p, ",") &&
        parse_inline_asm_operand_name(p) &&
        inline_asm_string_sequence_is(p, "=d", &p->pos) && expect(p, "(") &&
        parse_name(p, &edx_name) && expect(p, ")") && expect(p, ":") &&
        inline_asm_string_sequence_is(p, "c", &p->pos) && expect(p, "(");
    c_value_t index = {-1, C_BAD, NULL};
    if (operands_ok)
      index = parse_expr(p, 1);
    c_local_t *eax_local = find_local(p, eax_name);
    c_local_t *edx_local = find_local(p, edx_name);
    operands_ok = operands_ok && index.value >= 0 &&
                  c_type_is_integer(index.type) && expect(p, ")") &&
                  expect(p, ")") && expect(p, ";") && eax_local && edx_local &&
                  !eax_local->array_rank && !edx_local->array_rank &&
                  !eax_local->is_const && !edx_local->is_const &&
                  c_type_is_integer(eax_local->type) &&
                  c_type_is_integer(edx_local->type);
    if (!operands_ok) {
      free(assembly);
      return false;
    }
    int eax_address =
        emit_value(p, (nyr_inst_t){.op = NYIR_ADDR_LOCAL,
                                   .dst = -1,
                                   .imm = (int64_t)eax_local->slot});
    int edx_address =
        emit_value(p, (nyr_inst_t){.op = NYIR_ADDR_LOCAL,
                                   .dst = -1,
                                   .imm = (int64_t)edx_local->slot});
    free(assembly);
    return eax_address >= 0 && edx_address >= 0 &&
           emit(p,
                (nyr_inst_t){.op = NYIR_INLINE_ASM,
                             .dst = -1,
                             .a = index.value,
                             .b = eax_address,
                             .c = edx_address,
                             .d = -1,
                             .e = -1,
                             .f = -1,
                             .imm = -125,
                             .symbol = "xgetbv",
                             .effects =
                                 NYIR_EFFECT_CONTROL | NYIR_EFFECT_VOLATILE},
                NULL);
  }
  if (fxrstor_user_template) {
    bool frstor_template = memmem(assembly, assembly_len, "frstor %[fx]",
                                  sizeof("frstor %[fx]") - 1) != NULL;
    ny_ctok_t error_name, result_local_name, input_name;
    if (!expect(p, ":") || !parse_named_inline_asm_operand(p, &error_name) ||
        !inline_asm_name_matches(error_name, (const unsigned char *)"err", 3) ||
        !inline_asm_string_sequence_is(p, "=a", &p->pos) || !expect(p, "(") ||
        !parse_name(p, &result_local_name) || !expect(p, ")") ||
        !expect(p, ",") || !parse_inline_asm_operand_name(p) ||
        !inline_asm_string_sequence_is(p, "=m", &p->pos) || !expect(p, "(")) {
      free(assembly);
      return false;
    }
    c_value_t output = parse_expr(p, 1);
    if (output.value < 0 || output.type != C_AGG || !expect(p, ")") ||
        !expect(p, ":") || !inline_asm_string_sequence_is(p, "0", &p->pos) ||
        !expect(p, "(")) {
      free(assembly);
      return false;
    }
    c_value_t zero = parse_expr(p, 1);
    if (zero.value < 0 || !expect(p, ")") || !expect(p, ",") ||
        !parse_named_inline_asm_operand(p, &input_name) ||
        !inline_asm_name_matches(input_name, (const unsigned char *)"fx", 2) ||
        !inline_asm_string_sequence_is(p, "m", &p->pos) || !expect(p, "(")) {
      free(assembly);
      return false;
    }
    c_value_t input = parse_expr(p, 1);
    c_local_t *result_local = find_local(p, result_local_name);
    if (input.value < 0 || input.type != C_AGG || !expect(p, ")") ||
        !expect(p, ")") || !expect(p, ";") || !result_local ||
        result_local->type != C_I32 || result_local->array_rank ||
        result_local->is_const) {
      free(assembly);
      return false;
    }
    int address = output.value;
    free(assembly);
    int result = emit_value(
        p,
        (nyr_inst_t){.op = NYIR_INLINE_ASM,
                     .dst = -1,
                     .a = address,
                     .b = -1,
                     .c = -1,
                     .d = -1,
                     .e = -1,
                     .f = -1,
                     .imm = -124,
                     .symbol = frstor_template ? "frstor_user" : "fxrstor_user",
                     .effects = NYIR_EFFECT_READ_MEMORY | NYIR_EFFECT_CONTROL |
                                NYIR_EFFECT_VOLATILE});
    return result >= 0 && lower_local_store(p, result_local, result);
  }
  if (fxrstor_error_template) {
    bool frstor_template = memmem(assembly, assembly_len, "frstor %[fx]",
                                  sizeof("frstor %[fx]") - 1) != NULL;
    ny_ctok_t error_name, input_name;
    if (!expect(p, ":") || !parse_named_inline_asm_operand(p, &error_name) ||
        !inline_asm_name_matches(error_name, (const unsigned char *)"err", 3) ||
        !inline_asm_string_sequence_is(p, "=r", &p->pos) || !expect(p, "(")) {
      free(assembly);
      return false;
    }
    ny_ctok_t result_local_name;
    if (!parse_name(p, &result_local_name) || !expect(p, ")") ||
        !expect(p, ",") || !parse_inline_asm_operand_name(p) ||
        !inline_asm_string_sequence_is(p, "=m", &p->pos) || !expect(p, "(")) {
      free(assembly);
      return false;
    }
    c_value_t output = parse_expr(p, 1);
    if (output.value < 0 || output.type != C_AGG || !expect(p, ")") ||
        !expect(p, ":") || !inline_asm_string_sequence_is(p, "0", &p->pos) ||
        !expect(p, "(")) {
      free(assembly);
      return false;
    }
    c_value_t zero = parse_expr(p, 1);
    if (zero.value < 0 || !expect(p, ")") || !expect(p, ",") ||
        !parse_named_inline_asm_operand(p, &input_name) ||
        !inline_asm_name_matches(input_name, (const unsigned char *)"fx", 2) ||
        !inline_asm_string_sequence_is(p, "m", &p->pos) || !expect(p, "(")) {
      free(assembly);
      return false;
    }
    c_value_t input = parse_expr(p, 1);
    c_local_t *result_local = find_local(p, result_local_name);
    if (input.value < 0 || input.type != C_AGG || !expect(p, ")") ||
        !expect(p, ")") || !expect(p, ";") || !result_local ||
        result_local->type != C_I32 || result_local->array_rank ||
        result_local->is_const) {
      free(assembly);
      return false;
    }
    int address = output.value;
    free(assembly);
    int result = emit_value(
        p, (nyr_inst_t){.op = NYIR_INLINE_ASM,
                        .dst = -1,
                        .a = address,
                        .b = -1,
                        .c = -1,
                        .d = -1,
                        .e = -1,
                        .f = -1,
                        .imm = -123,
                        .symbol =
                            frstor_template ? "frstor_error" : "fxrstor_error",
                        .effects = NYIR_EFFECT_READ_MEMORY |
                                   NYIR_EFFECT_CONTROL | NYIR_EFFECT_VOLATILE});
    if (result < 0)
      return false;
    return lower_local_store(p, result_local, result);
  }
  if (fxrstor_extable_template) {
    bool frstor_template = memmem(assembly, assembly_len, "frstor %[fx]",
                                  sizeof("frstor %[fx]") - 1) != NULL;
    ny_ctok_t input_name;
    if (!expect(p, ":") || !parse_inline_asm_operand_name(p) ||
        !inline_asm_string_sequence_is(p, "=m", &p->pos) || !expect(p, "(")) {
      free(assembly);
      return false;
    }
    c_value_t output = parse_expr(p, 1);
    int address = output.value >= 0 && output.type == C_AGG ? output.value : -1;
    if (address < 0 || !expect(p, ")") || !expect(p, ":") ||
        !parse_named_inline_asm_operand(p, &input_name) ||
        !inline_asm_name_matches(input_name, (const unsigned char *)"fx", 2) ||
        !inline_asm_string_sequence_is(p, "m", &p->pos) || !expect(p, "(")) {
      free(assembly);
      return false;
    }
    c_value_t input = parse_expr(p, 1);
    if (input.value < 0 || input.type != C_AGG || !expect(p, ")") ||
        !expect(p, ")") || !expect(p, ";")) {
      free(assembly);
      return false;
    }
    free(assembly);
    return emit(p,
                (nyr_inst_t){.op = NYIR_INLINE_ASM,
                             .dst = -1,
                             .a = address,
                             .b = -1,
                             .c = -1,
                             .d = -1,
                             .e = -1,
                             .f = -1,
                             .imm = -122,
                             .symbol = frstor_template ? "frstor_extable"
                                                       : "fxrstor_extable",
                             .effects = NYIR_EFFECT_READ_MEMORY |
                                        NYIR_EFFECT_CONTROL |
                                        NYIR_EFFECT_VOLATILE},
                NULL);
  }
  if (!asm_goto && ldmxcsr_template) {
    if (!expect(p, ":") || !expect(p, ":") ||
        !inline_asm_string_sequence_is(p, "m", &p->pos) || !expect(p, "(")) {
      free(assembly);
      return false;
    }
    ny_ctok_t input_name;
    if (!parse_name(p, &input_name)) {
      free(assembly);
      return false;
    }
    c_local_t *local = find_local(p, input_name);
    if (!local || local->array_rank || !expect(p, ")") || !expect(p, ")") ||
        !expect(p, ";")) {
      free(assembly);
      return false;
    }
    int address = emit_value(p, (nyr_inst_t){.op = NYIR_ADDR_LOCAL,
                                             .dst = -1,
                                             .imm = (int64_t)local->slot});
    if (address < 0) {
      free(assembly);
      return false;
    }
    free(assembly);
    return emit(
        p,
        (nyr_inst_t){.op = NYIR_INLINE_ASM,
                     .dst = -1,
                     .a = address,
                     .b = -1,
                     .c = -1,
                     .d = -1,
                     .e = -1,
                     .f = -1,
                     .imm = -121,
                     .symbol = "ldmxcsr",
                     .effects = NYIR_EFFECT_READ_MEMORY | NYIR_EFFECT_VOLATILE},
        NULL);
  }
  if (!asm_goto && fildl_clear_template) {
    ny_ctok_t address_name;
    if (!expect(p, ":") || !expect(p, ":") ||
        !parse_named_inline_asm_operand(p, &address_name) ||
        !inline_asm_name_matches(address_name, (const unsigned char *)"addr",
                                 4) ||
        !inline_asm_string_sequence_is(p, "m", &p->pos) || !expect(p, "(")) {
      free(assembly);
      return false;
    }
    bool dereference_address = accept(p, "*");
    c_value_t value = parse_expr(p, 1);
    const c_lvalue_t *lvalue = !dereference_address && value.value >= 0
                                   ? find_lvalue(p, value.value)
                                   : NULL;
    int address = dereference_address
                      ? (c_type_is_pointer(value.type) ? value.value : -1)
                  : lvalue ? lvalue->address
                           : -1;
    if (address < 0 || !expect(p, ")") || !expect(p, ")") || !expect(p, ";")) {
      free(assembly);
      return false;
    }
    free(assembly);
    return emit(p,
                (nyr_inst_t){.op = NYIR_INLINE_ASM,
                             .dst = -1,
                             .a = address,
                             .b = -1,
                             .c = -1,
                             .d = -1,
                             .e = -1,
                             .f = -1,
                             .imm = -120,
                             .symbol = "fildl_clear",
                             .effects = NYIR_EFFECT_WRITE_MEMORY |
                                        NYIR_EFFECT_CONTROL |
                                        NYIR_EFFECT_VOLATILE},
                NULL);
  }
  if (!asm_goto && fdiv_bug_template) {
    int addresses[3] = {-1, -1, -1};
    bool operands_ok = expect(p, ":") &&
                       inline_asm_string_sequence_is(p, "=m", &p->pos) &&
                       expect(p, "(");
    if (operands_ok) {
      bool dereference_address = accept(p, "*");
      c_value_t value = parse_expr(p, 1);
      const c_lvalue_t *lvalue = !dereference_address && value.value >= 0
                                     ? find_lvalue(p, value.value)
                                     : NULL;
      int address = dereference_address
                        ? (c_type_is_pointer(value.type) ? value.value : -1)
                    : lvalue ? lvalue->address
                             : -1;
      operands_ok = address >= 0 && expect(p, ")");
      if (operands_ok)
        addresses[0] = address;
    }
    operands_ok = operands_ok && expect(p, ":");
    for (size_t i = 1; operands_ok && i < 3; ++i) {
      operands_ok = (i == 1 || expect(p, ",")) &&
                    inline_asm_string_sequence_is(p, "m", &p->pos) &&
                    expect(p, "(");
      if (operands_ok) {
        bool dereference_address = accept(p, "*");
        c_value_t value = parse_expr(p, 1);
        const c_lvalue_t *lvalue = !dereference_address && value.value >= 0
                                       ? find_lvalue(p, value.value)
                                       : NULL;
        int address = dereference_address
                          ? (c_type_is_pointer(value.type) ? value.value : -1)
                      : lvalue ? lvalue->address
                               : -1;
        operands_ok = address >= 0 && expect(p, ")");
        if (operands_ok)
          addresses[i] = address;
      }
    }
    operands_ok = operands_ok && expect(p, ")") && expect(p, ";");
    if (!operands_ok) {
      fail(p, "unsupported x87 FDIV probe operands");
      free(assembly);
      return false;
    }
    free(assembly);
    return emit(p,
                (nyr_inst_t){.op = NYIR_INLINE_ASM,
                             .dst = -1,
                             .a = addresses[0],
                             .b = addresses[1],
                             .c = addresses[2],
                             .d = -1,
                             .e = -1,
                             .f = -1,
                             .imm = -119,
                             .symbol = "fdiv_bug_probe",
                             .effects = NYIR_EFFECT_READ_MEMORY |
                                        NYIR_EFFECT_WRITE_MEMORY |
                                        NYIR_EFFECT_CONTROL |
                                        NYIR_EFFECT_VOLATILE},
                NULL);
  }
  if (!asm_goto && fninit_status_template) {
    int addresses[2] = {-1, -1};
    bool operands_ok = expect(p, ":");
    for (size_t i = 0; operands_ok && i < 2; ++i) {
      ny_ctok_t local_name;
      operands_ok = (!i || expect(p, ",")) &&
                    parse_inline_asm_operand_name(p) &&
                    inline_asm_string_sequence_is(p, "+m", &p->pos) &&
                    expect(p, "(") && parse_name(p, &local_name);
      if (operands_ok) {
        c_local_t *local = find_local(p, local_name);
        operands_ok = local && !local->array_rank && !local->is_const &&
                      c_type_is_integer(local->type) && expect(p, ")");
        if (operands_ok) {
          local->has_integer_constant = false;
          local->address_taken = true;
          addresses[i] =
              emit_value(p, (nyr_inst_t){.op = NYIR_ADDR_LOCAL,
                                         .dst = -1,
                                         .imm = (int64_t)local->slot});
        }
        operands_ok = operands_ok && addresses[i] >= 0;
      }
    }
    operands_ok = operands_ok && expect(p, ")") && expect(p, ";");
    if (!operands_ok) {
      fail(p, "unsupported fninit x87 status operands");
      free(assembly);
      return false;
    }
    free(assembly);
    return emit(p,
                (nyr_inst_t){.op = NYIR_INLINE_ASM,
                             .dst = -1,
                             .a = addresses[0],
                             .b = addresses[1],
                             .c = -1,
                             .d = -1,
                             .e = -1,
                             .f = -1,
                             .imm = -118,
                             .symbol = "fninit_status",
                             .effects = NYIR_EFFECT_WRITE_MEMORY |
                                        NYIR_EFFECT_VOLATILE |
                                        NYIR_EFFECT_CONTROL},
                NULL);
  }
  if (!asm_goto && fxsave_mnemonic) {
    if (!expect(p, ":") || !parse_inline_asm_operand_name(p)) {
      free(assembly);
      return false;
    }
    size_t constraint_end = p->pos;
    if (!inline_asm_string_sequence_is(p, "=m", &constraint_end) &&
        !inline_asm_string_sequence_is(p, "+m", &constraint_end)) {
      free(assembly);
      return false;
    }
    p->pos = constraint_end;
    if (!expect(p, "(")) {
      free(assembly);
      return false;
    }
    c_value_t output = parse_expr(p, 1);
    const c_lvalue_t *lvalue =
        output.value >= 0 ? find_lvalue(p, output.value) : NULL;
    int address = lvalue && lvalue->address >= 0 ? lvalue->address
                  : output.type == C_AGG         ? output.value
                                                 : -1;
    if (address < 0 || !expect(p, ")") || !expect(p, ")") || !expect(p, ";")) {
      fail(p, "unsupported fxsave memory-output operand");
      free(assembly);
      return false;
    }
    free(assembly);
    return emit(p,
                (nyr_inst_t){.op = NYIR_INLINE_ASM,
                             .dst = -1,
                             .a = address,
                             .b = -1,
                             .c = -1,
                             .d = -1,
                             .e = -1,
                             .f = -1,
                             .imm = -117,
                             .symbol = fxsave_mnemonic,
                             .effects = NYIR_EFFECT_WRITE_MEMORY |
                                        NYIR_EFFECT_VOLATILE},
                NULL);
  }
  bool clflush_alternative_template =
      !asm_goto && INLINE_ASM_HAS("# ALT: oldinstr") &&
      INLINE_ASM_HAS("clflush (%[addr])") && INLINE_ASM_HAS(".altinstructions");
  bool clflushopt_alternative_template =
      !asm_goto && INLINE_ASM_HAS("# ALT: oldinstr") &&
      INLINE_ASM_HAS("ds clflush %0") && INLINE_ASM_HAS("clflushopt %0") &&
      INLINE_ASM_HAS(".altinstructions");
  const char *monitor_name =
      assembly_len == sizeof("monitor") - 1 &&
              memcmp(assembly, "monitor", sizeof("monitor") - 1) == 0
          ? "monitor"
      : assembly_len == sizeof("monitorx") - 1 &&
              memcmp(assembly, "monitorx", sizeof("monitorx") - 1) == 0
          ? "monitorx"
      : assembly_len == sizeof("mwaitx") - 1 &&
              memcmp(assembly, "mwaitx", sizeof("mwaitx") - 1) == 0
          ? "mwaitx"
          : NULL;
  if (!asm_goto && monitor_name) {
    static const char *const monitor_constraints[] = {"a", "c", "d"};
    static const char *const mwaitx_constraints[] = {"a", "b", "c"};
    const char *const *constraints = strcmp(monitor_name, "mwaitx") == 0
                                         ? mwaitx_constraints
                                         : monitor_constraints;
    int inputs[3] = {-1, -1, -1};
    bool operands_ok = expect(p, ":") && expect(p, ":");
    for (size_t i = 0; operands_ok && i < 3; ++i) {
      operands_ok = (!i || expect(p, ",")) &&
                    inline_asm_string_sequence_is(p, constraints[i], &p->pos) &&
                    expect(p, "(");
      if (operands_ok) {
        c_value_t input = parse_expr(p, 1);
        operands_ok = input.value >= 0 &&
                      (i == 0 && strcmp(monitor_name, "mwaitx") != 0
                           ? c_type_is_pointer(input.type)
                           : c_type_is_integer(input.type)) &&
                      expect(p, ")");
        if (operands_ok)
          inputs[i] = input.value;
      }
    }
    operands_ok = operands_ok && expect(p, ")") && expect(p, ";");
    if (!operands_ok) {
      free(assembly);
      return false;
    }
    free(assembly);
    return emit(
        p,
        (nyr_inst_t){.op = NYIR_INLINE_ASM,
                     .dst = -1,
                     .a = inputs[0],
                     .b = inputs[1],
                     .c = inputs[2],
                     .d = -1,
                     .e = -1,
                     .f = -1,
                     .imm = -116,
                     .symbol = monitor_name,
                     .effects = NYIR_EFFECT_CONTROL | NYIR_EFFECT_VOLATILE},
        NULL);
  }
  if (clflush_memory_template) {
    if (!expect(p, ":") || !inline_asm_string_sequence_is(p, "+m", &p->pos) ||
        !expect(p, "(")) {
      free(assembly);
      return false;
    }
    c_value_t memory = parse_expr(p, 1);
    const c_lvalue_t *lvalue =
        memory.value >= 0 ? find_lvalue(p, memory.value) : NULL;
    if (!lvalue || lvalue->address < 0 || !expect(p, ")") || !expect(p, ")") ||
        !expect(p, ";")) {
      free(assembly);
      return false;
    }
    int address = lvalue->address;
    free(assembly);
    return emit(p,
                (nyr_inst_t){.op = NYIR_INLINE_ASM,
                             .dst = -1,
                             .a = address,
                             .b = -1,
                             .c = -1,
                             .d = -1,
                             .e = -1,
                             .f = -1,
                             .imm = -115,
                             .symbol = "clflush",
                             .effects = NYIR_EFFECT_WRITE_MEMORY |
                                        NYIR_EFFECT_VOLATILE},
                NULL);
  }
  if (clflush_alternative_template) {
    ny_ctok_t address_name;
    if (!expect(p, ":") || !expect(p, ":") ||
        !inline_asm_string_sequence_is(p, "i", &p->pos) || !expect(p, "(")) {
      free(assembly);
      return false;
    }
    c_value_t placeholder = parse_expr(p, 1);
    if (placeholder.value < 0 || !c_type_is_integer(placeholder.type) ||
        !expect(p, ")") || !expect(p, ",") ||
        !parse_named_inline_asm_operand(p, &address_name) ||
        !inline_asm_name_matches(address_name, (const unsigned char *)"addr",
                                 4) ||
        !inline_asm_string_sequence_is(p, "a", &p->pos) || !expect(p, "(")) {
      free(assembly);
      return false;
    }
    c_value_t address = parse_expr(p, 1);
    if (address.value < 0 || !c_type_is_pointer(address.type) ||
        !expect(p, ")") || !expect(p, ")") || !expect(p, ";")) {
      free(assembly);
      return false;
    }
    free(assembly);
    return emit(p,
                (nyr_inst_t){.op = NYIR_INLINE_ASM,
                             .dst = -1,
                             .a = address.value,
                             .b = -1,
                             .c = -1,
                             .d = -1,
                             .e = -1,
                             .f = -1,
                             .imm = -115,
                             .symbol = "clflush",
                             .effects = NYIR_EFFECT_WRITE_MEMORY |
                                        NYIR_EFFECT_VOLATILE},
                NULL);
  }
  if (clflushopt_alternative_template) {
    if (!expect(p, ":") || !inline_asm_string_sequence_is(p, "+m", &p->pos) ||
        !expect(p, "(")) {
      fail(p, "invalid clflushopt alternative output constraint");
      free(assembly);
      return false;
    }
    c_value_t memory = parse_expr(p, 1);
    const c_lvalue_t *lvalue =
        memory.value >= 0 ? find_lvalue(p, memory.value) : NULL;
    int address = lvalue ? lvalue->address : -1;
    /*
     * GNU memory operands over an explicitly cast pointer may retain the
     * pointer as their value without materializing a scalar lvalue address.
     */
    if (address < 0 && memory.value >= 0)
      address = memory.value;
    if (address < 0 || !expect(p, ")")) {
      fail(p, "invalid clflushopt alternative output operand");
      free(assembly);
      return false;
    }
    if (accept(p, ":")) {
      if (!inline_asm_string_sequence_is(p, "i", &p->pos) || !expect(p, "(")) {
        fail(p, "invalid clflushopt alternative input constraint");
        free(assembly);
        return false;
      }
      c_value_t placeholder = parse_expr(p, 1);
      if (placeholder.value < 0 || placeholder.type == C_BAD ||
          !expect(p, ")")) {
        fail(p, "invalid clflushopt alternative input operand");
        free(assembly);
        return false;
      }
    }
    if (!expect(p, ")") || !expect(p, ";")) {
      fail(p, "invalid clflushopt alternative closing tokens");
      free(assembly);
      return false;
    }
    free(assembly);
    return emit(p,
                (nyr_inst_t){.op = NYIR_INLINE_ASM,
                             .dst = -1,
                             .a = address,
                             .b = -1,
                             .c = -1,
                             .d = -1,
                             .e = -1,
                             .f = -1,
                             .imm = -115,
                             .symbol = "clflush",
                             .effects = NYIR_EFFECT_WRITE_MEMORY |
                                        NYIR_EFFECT_VOLATILE},
                NULL);
  }
  if (paravirt_void_call_template) {
    static const char *const output_constraints_64[] = {"=D", "=S", "=d", "=c"};
    static const char *const output_constraints_32[] = {"=a", "=d", "=c"};
    bool operands_ok = expect(p, ":");
    size_t output_probe = p->pos;
    bool x86_32 = inline_asm_string_sequence_is(p, output_constraints_32[0],
                                                &output_probe);
    const char *const *output_constraints =
        x86_32 ? output_constraints_32 : output_constraints_64;
    size_t output_count = x86_32 ? 3 : 4;
    for (size_t i = 0; operands_ok && i < output_count; ++i) {
      ny_ctok_t ignored;
      operands_ok =
          (!i || expect(p, ",")) && parse_inline_asm_operand_name(p) &&
          inline_asm_string_sequence_is(p, output_constraints[i], &p->pos) &&
          expect(p, "(") && parse_name(p, &ignored) && expect(p, ")");
    }
    if (operands_ok) {
      ny_ctok_t ignored;
      operands_ok = expect(p, ",") && parse_inline_asm_operand_name(p) &&
                    inline_asm_string_sequence_is(p, "+r", &p->pos) &&
                    expect(p, "(") && parse_name(p, &ignored) && expect(p, ")");
    }
    ny_ctok_t input_name;
    c_value_t target = {-1, C_BAD, NULL};
    int call_args[5] = {-1, -1, -1, -1, -1};
    size_t call_argc = 0;
    operands_ok =
        operands_ok && expect(p, ":") &&
        parse_named_inline_asm_operand(p, &input_name) &&
        inline_asm_name_matches(input_name,
                                (const unsigned char *)"paravirt_opptr",
                                sizeof("paravirt_opptr") - 1) &&
        inline_asm_string_sequence_is(p, "m", &p->pos) && expect(p, "(");
    if (operands_ok)
      target = parse_expr(p, 1);
    operands_ok = operands_ok && target.value >= 0 &&
                  c_type_is_pointer(target.type) && expect(p, ")");
    while (operands_ok && accept(p, ",")) {
      static const char *const argument_constraints_64[] = {"D", "S", "d", "c"};
      static const char *const argument_constraints_32[] = {"a", "d", "c"};
      const char *const *argument_constraints =
          x86_32 ? argument_constraints_32 : argument_constraints_64;
      size_t argument_constraint_count = x86_32 ? 3 : 4;
      size_t constraint_end = p->pos;
      bool matched = false;
      for (size_t i = 0; i < argument_constraint_count; ++i) {
        if (inline_asm_string_sequence_is(p, argument_constraints[i],
                                          &constraint_end)) {
          matched = true;
          break;
        }
      }
      if (!matched || call_argc == 4) {
        operands_ok = false;
        break;
      }
      p->pos = constraint_end;
      if (!expect(p, "(")) {
        operands_ok = false;
        break;
      }
      c_value_t argument = parse_expr(p, 1);
      if (argument.value < 0 || !expect(p, ")")) {
        operands_ok = false;
        break;
      }
      call_args[call_argc++] = argument.value;
    }
    operands_ok = operands_ok && expect(p, ":");
    if (operands_ok) {
      size_t clobber_end = p->pos;
      operands_ok = inline_asm_clobber_list_is(p, &clobber_end);
      if (operands_ok)
        p->pos = clobber_end;
    }
    operands_ok = operands_ok && expect(p, ")") && expect(p, ";");
    if (!operands_ok) {
      free(assembly);
      return false;
    }
    /*
     * Keep the kernel's own call-plus-alternative text when the called slot
     * is a plain `symbol + offset`: the call then carries the `pv_ops`
     * relocation objtool wants, and boot-time patching turns it into a direct
     * call. Otherwise fall back to a call through the loaded pointer.
     */
    char *pv_text = NULL;
    {
      char member[96];
      int64_t member_offset = 0;
      static const char placeholder[] = "%[paravirt_opptr]";
      const char *at = assembly ? strstr((const char *)assembly, placeholder)
                                : NULL;
      if (at && !getenv("NYC_NO_PV_TEMPLATE") && p->abi != NYC_ABI_I386 &&
          paravirt_member_address(p, target.value, member, sizeof(member),
                                  &member_offset)) {
        char operand[160];
        if (member_offset)
          snprintf(operand, sizeof(operand), "%s+%lld(%%rip)", member,
                   (long long)member_offset);
        else
          snprintf(operand, sizeof(operand), "%s(%%rip)", member);
        size_t head = (size_t)(at - (const char *)assembly);
        size_t tail = strlen(at + sizeof(placeholder) - 1);
        pv_text = malloc(head + strlen(operand) + tail + 1);
        if (pv_text) {
          memcpy(pv_text, assembly, head);
          strcpy(pv_text + head, operand);
          strcpy(pv_text + head + strlen(operand), at + sizeof(placeholder) - 1);
          if (strchr(pv_text, '%') && strstr(pv_text, "%[")) {
            free(pv_text);
            pv_text = NULL;
          }
        }
      }
    }
    free(assembly);
    nyr_inst_t call = {.op = NYIR_CALL,
                       .dst = -1,
                       .a = target.value,
                       .b = call_args[0],
                       .c = call_args[1],
                       .d = call_args[2],
                       .e = call_args[3],
                       .imm = (int64_t)(call_argc + 1),
                       .flags = NYIR_INST_F_INDIRECT,
                       .effects = NYIR_EFFECT_CALL | NYIR_EFFECT_READ_MEMORY |
                                  NYIR_EFFECT_WRITE_MEMORY |
                                  NYIR_EFFECT_VOLATILE};
    nyr_call_arg_class_set(call.arg_scalar_classes, 0, NYIR_SCALAR_I64_RAW);
    for (size_t i = 0; i < call_argc; ++i)
      nyr_call_arg_class_set(call.arg_scalar_classes, i + 1,
                             NYIR_SCALAR_I64_RAW);
    if (pv_text) {
      call.symbol = pv_text;
      call.flags |= NYIR_INST_F_PV_TEMPLATE;
    }
    bool emitted_call = emit(p, call, NULL);
    free(pv_text);
    return emitted_call;
  }
  if (!asm_goto && wrmsr_alternative_template) {
    if (!expect(p, ":") || !expect(p, ":") ||
        !inline_asm_string_sequence_is(p, "c", &p->pos) || !expect(p, "("))
      return defer_unsupported_asm(p, open, assembly);
    c_value_t msr = parse_expr(p, 1);
    if (msr.value < 0 || !c_type_is_integer(msr.type) || !expect(p, ")") ||
        !expect(p, ",") || !inline_asm_string_sequence_is(p, "a", &p->pos) ||
        !expect(p, "("))
      return defer_unsupported_asm(p, open, assembly);
    c_value_t low = parse_expr(p, 1);
    if (low.value < 0 || !c_type_is_integer(low.type) || !expect(p, ")") ||
        !expect(p, ",") || !inline_asm_string_sequence_is(p, "d", &p->pos) ||
        !expect(p, "("))
      return defer_unsupported_asm(p, open, assembly);
    c_value_t high = parse_expr(p, 1);
    ny_ctok_t feature_name;
    if (high.value < 0 || !c_type_is_integer(high.type) || !expect(p, ")") ||
        !expect(p, ",") || !parse_named_inline_asm_operand(p, &feature_name) ||
        !inline_asm_name_matches(feature_name, (const unsigned char *)"feature",
                                 7) ||
        !inline_asm_string_sequence_is(p, "i", &p->pos) || !expect(p, "("))
      return defer_unsupported_asm(p, open, assembly);
    c_value_t feature = parse_expr(p, 1);
    if (feature.value < 0 || !c_type_is_integer(feature.type) ||
        !expect(p, ")") || !expect(p, ":") ||
        !inline_asm_string_sequence_is(p, "memory", &p->pos) ||
        !expect(p, ")") || !expect(p, ";"))
      return defer_unsupported_asm(p, open, assembly);
    free(assembly);
    return emit(p,
                (nyr_inst_t){
                    .op = NYIR_INLINE_ASM,
                    .dst = -1,
                    .a = msr.value,
                    .b = low.value,
                    .c = high.value,
                    .d = feature.value,
                    .imm = -100,
                    .effects = NYIR_EFFECT_READ_MEMORY |
                               NYIR_EFFECT_WRITE_MEMORY | NYIR_EFFECT_MAY_TRAP |
                               NYIR_EFFECT_CONTROL | NYIR_EFFECT_VOLATILE,
                    .symbol = "alternative_wrmsr"},
                NULL);
  }

  bool wrmsr_safe_template = INLINE_ASM_HAS("1: wrmsr") &&
                             INLINE_ASM_HAS("xor %[err],%[err]") &&
                             INLINE_ASM_HAS(".macro extable_type_reg");
  if (!asm_goto && wrmsr_safe_template) {
    ny_ctok_t error_name;
    if (!expect(p, ":") || !parse_named_inline_asm_operand(p, &error_name) ||
        !inline_asm_string_sequence_is(p, "=a", &p->pos) || !expect(p, "(") ||
        !parse_name(p, &error_name) || !expect(p, ")") || !expect(p, ":") ||
        !inline_asm_string_sequence_is(p, "c", &p->pos) || !expect(p, "(")) {
      free(assembly);
      return false;
    }
    c_value_t msr = parse_expr(p, 1);
    if (msr.value < 0 || !c_type_is_integer(msr.type) || !expect(p, ")") ||
        !expect(p, ",") || !inline_asm_string_sequence_is(p, "0", &p->pos) ||
        !expect(p, "(")) {
      free(assembly);
      return false;
    }
    c_value_t low = parse_expr(p, 1);
    if (low.value < 0 || !c_type_is_integer(low.type) || !expect(p, ")") ||
        !expect(p, ",") || !inline_asm_string_sequence_is(p, "d", &p->pos) ||
        !expect(p, "(")) {
      free(assembly);
      return false;
    }
    c_value_t high = parse_expr(p, 1);
    c_local_t *error_local = find_local(p, error_name);
    if (high.value < 0 || !c_type_is_integer(high.type) || !expect(p, ")") ||
        !expect(p, ":") ||
        !inline_asm_string_sequence_is(p, "memory", &p->pos) ||
        !expect(p, ")") || !expect(p, ";") || !error_local ||
        error_local->type != C_I32 || error_local->is_const) {
      free(assembly);
      return false;
    }
    free(assembly);
    error_local->address_taken = true;
    int error_address = emit_value(p, (nyr_inst_t){.op = NYIR_ADDR_LOCAL,
                                                   .dst = -1,
                                                   .imm = error_local->slot});
    if (error_address < 0)
      return false;
    char slots[64];
    int slots_len = snprintf(slots, sizeof(slots), "wrmsr_safe;nyc_slots=%zu",
                             error_local->slot);
    if (slots_len < 0 || (size_t)slots_len >= sizeof(slots)) {
      fail(p, "out of memory lowering safe wrmsr inline assembly");
      return false;
    }
    const char *lowered_template = nyr_func_own_symbol_copy(p->out, slots);
    if (!lowered_template) {
      fail(p, "out of memory lowering safe wrmsr inline assembly");
      return false;
    }
    return emit(p,
                (nyr_inst_t){
                    .op = NYIR_INLINE_ASM,
                    .dst = -1,
                    .a = msr.value,
                    .b = low.value,
                    .c = high.value,
                    .d = error_address,
                    .imm = -29,
                    .symbol = lowered_template,
                    .effects = NYIR_EFFECT_WRITE_MEMORY | NYIR_EFFECT_MAY_TRAP |
                               NYIR_EFFECT_CONTROL | NYIR_EFFECT_VOLATILE},
                NULL);
  }

  bool rdpmc_template = !asm_goto && assembly_len == sizeof("rdpmc") - 1 &&
                        memcmp(assembly, "rdpmc", sizeof("rdpmc") - 1) == 0;
  if (rdpmc_template && p->abi == NYC_ABI_I386) {
    ny_ctok_t value_name;
    if (!expect(p, ":") || !inline_asm_string_sequence_is(p, "=A", &p->pos) ||
        !expect(p, "(") || !parse_name(p, &value_name) || !expect(p, ")") ||
        !expect(p, ":") || !inline_asm_string_sequence_is(p, "c", &p->pos) ||
        !expect(p, "(")) {
      free(assembly);
      return false;
    }
    c_value_t counter = parse_expr(p, 1);
    c_local_t *value_local = find_local(p, value_name);
    if (counter.value < 0 || !c_type_is_integer(counter.type) ||
        !expect(p, ")") || !expect(p, ")") || !expect(p, ";") || !value_local ||
        (value_local->type != C_U64 && value_local->type != C_I64) ||
        value_local->is_const || value_local->array_rank) {
      fail(p, "unsupported i386 rdpmc output operands");
      free(assembly);
      return false;
    }
    free(assembly);
    value_local->address_taken = true;
    int value_address =
        emit_value(p, (nyr_inst_t){.op = NYIR_ADDR_LOCAL,
                                   .dst = -1,
                                   .imm = (int64_t)value_local->slot,
                                   .scalar_class = NYIR_SCALAR_I64_RAW});
    if (value_address < 0)
      return false;
    char slots[64];
    int slots_len = snprintf(slots, sizeof(slots), "rdpmc_i386;nyc_slot=%zu",
                             value_local->slot);
    if (slots_len < 0 || (size_t)slots_len >= sizeof(slots)) {
      fail(p, "out of memory lowering i386 rdpmc inline assembly");
      return false;
    }
    const char *lowered_template = nyr_func_own_symbol_copy(p->out, slots);
    if (!lowered_template) {
      fail(p, "out of memory lowering i386 rdpmc inline assembly");
      return false;
    }
    return emit(p,
                (nyr_inst_t){.op = NYIR_INLINE_ASM,
                             .dst = -1,
                             .a = counter.value,
                             .b = -1,
                             .c = -1,
                             .d = value_address,
                             .e = -1,
                             .imm = -104,
                             .symbol = lowered_template,
                             .effects = NYIR_EFFECT_READ_MEMORY |
                                        NYIR_EFFECT_WRITE_MEMORY |
                                        NYIR_EFFECT_CONTROL |
                                        NYIR_EFFECT_VOLATILE},
                NULL);
  }
  if (rdpmc_template) {
    ny_ctok_t low_name, high_name;
    if (!expect(p, ":") || !inline_asm_string_sequence_is(p, "=a", &p->pos) ||
        !expect(p, "(") || !parse_name(p, &low_name) || !expect(p, ")") ||
        !expect(p, ",") || !inline_asm_string_sequence_is(p, "=d", &p->pos) ||
        !expect(p, "(") || !parse_name(p, &high_name) || !expect(p, ")") ||
        !expect(p, ":") || !inline_asm_string_sequence_is(p, "c", &p->pos) ||
        !expect(p, "(")) {
      free(assembly);
      return false;
    }
    c_value_t counter = parse_expr(p, 1);
    c_local_t *low_local = find_local(p, low_name);
    c_local_t *high_local = find_local(p, high_name);
    if (counter.value < 0 || !c_type_is_integer(counter.type) ||
        !expect(p, ")") || !expect(p, ")") || !expect(p, ";") || !low_local ||
        !high_local || low_local == high_local ||
        !c_type_is_integer(low_local->type) ||
        !c_type_is_integer(high_local->type) || low_local->is_const ||
        high_local->is_const) {
      free(assembly);
      return false;
    }
    free(assembly);
    low_local->address_taken = true;
    high_local->address_taken = true;
    int low_address = emit_value(
        p,
        (nyr_inst_t){.op = NYIR_ADDR_LOCAL, .dst = -1, .imm = low_local->slot});
    int high_address = emit_value(p, (nyr_inst_t){.op = NYIR_ADDR_LOCAL,
                                                  .dst = -1,
                                                  .imm = high_local->slot});
    if (low_address < 0 || high_address < 0)
      return false;
    char slots[64];
    int slots_len = snprintf(slots, sizeof(slots), "rdpmc;nyc_slots=%zu,%zu",
                             low_local->slot, high_local->slot);
    if (slots_len < 0 || (size_t)slots_len >= sizeof(slots)) {
      fail(p, "out of memory lowering rdpmc inline assembly");
      return false;
    }
    const char *lowered_template = nyr_func_own_symbol_copy(p->out, slots);
    if (!lowered_template) {
      fail(p, "out of memory lowering rdpmc inline assembly");
      return false;
    }
    return emit(
        p,
        (nyr_inst_t){.op = NYIR_INLINE_ASM,
                     .dst = -1,
                     .a = counter.value,
                     .b = -1,
                     .c = -1,
                     .d = low_address,
                     .e = high_address,
                     .imm = -30,
                     .symbol = lowered_template,
                     .effects = NYIR_EFFECT_READ_MEMORY | NYIR_EFFECT_MAY_TRAP |
                                NYIR_EFFECT_CONTROL | NYIR_EFFECT_VOLATILE},
        NULL);
  }

  static const char local_add_asm[] = " addq %1,%0";
  static const char local_sub_asm[] = " subq %1,%0";
  bool local_add =
      assembly_len == sizeof(local_add_asm) - 1 &&
      memcmp(assembly, local_add_asm, sizeof(local_add_asm) - 1) == 0;
  bool local_sub =
      assembly_len == sizeof(local_sub_asm) - 1 &&
      memcmp(assembly, local_sub_asm, sizeof(local_sub_asm) - 1) == 0;
  if (!asm_goto && (local_add || local_sub)) {
    size_t constraint_end = p->pos;
    if (!expect(p, ":") ||
        !inline_asm_string_sequence_is(p, "+m", &constraint_end)) {
      free(assembly);
      return false;
    }
    p->pos = constraint_end;
    if (!expect(p, "(")) {
      free(assembly);
      return false;
    }
    c_value_t output = parse_expr(p, 1);
    const c_lvalue_t *output_lvalue =
        output.value >= 0 ? find_lvalue(p, output.value) : NULL;
    if (!output_lvalue || output_lvalue->address < 0 ||
        (output_lvalue->type != C_I64 && output_lvalue->type != C_U64) ||
        !expect(p, ")") || !discard_lvalue_read(p, output_lvalue) ||
        !expect(p, ":")) {
      free(assembly);
      return false;
    }
    constraint_end = p->pos;
    if (!inline_asm_string_sequence_is(p, "ir", &constraint_end)) {
      free(assembly);
      return false;
    }
    p->pos = constraint_end;
    if (!expect(p, "(")) {
      free(assembly);
      return false;
    }
    c_value_t input = parse_expr(p, 1);
    if (input.value < 0 || !c_type_is_integer(input.type) || !expect(p, ")") ||
        !expect(p, ")") || !expect(p, ";")) {
      free(assembly);
      return false;
    }
    free(assembly);
    int old = lower_load_field(p, output_lvalue->address, false, true);
    int addend = input.type == output_lvalue->type
                     ? input.value
                     : normalize_integer(p, output_lvalue->type, input.value);
    int updated =
        old < 0 || addend < 0
            ? -1
            : emit_value(
                  p, (nyr_inst_t){.op = local_add ? NYIR_ADD_I64 : NYIR_SUB_I64,
                                  .dst = -1,
                                  .a = old,
                                  .b = addend,
                                  .scalar_class = NYIR_SCALAR_I64_RAW});
    if (updated < 0 ||
        !lower_store_field(p, output_lvalue->address, updated, false, true))
      return false;
    return true;
  }

  static const char lkgs_opcode[] = ".byte 0xf2,0x0f,0x00,0xf7";
  static const char lkgs_extable[] = ".macro extable_type_reg";
  bool lkgs_asm =
      !asm_goto && INLINE_ASM_HAS(lkgs_opcode) && INLINE_ASM_HAS(lkgs_extable);
  if (lkgs_asm) {
    ny_ctok_t selector_name;
    if (!expect(p, ":") || !parse_inline_asm_operand_name(p) ||
        !inline_asm_string_sequence_is(p, "+D", &p->pos) || !expect(p, "(") ||
        !parse_name(p, &selector_name) || !expect(p, ")") || !expect(p, ")") ||
        !expect(p, ";"))
      return defer_unsupported_asm(p, open, assembly);
    c_local_t *selector_local = find_local(p, selector_name);
    if (!selector_local || selector_local->array_rank ||
        selector_local->is_const || selector_local->type != C_U16)
      return defer_unsupported_asm(p, open, assembly);
    int selector = lower_local_load(p, selector_local);
    free(assembly);
    if (selector < 0)
      return false;
    return emit(p,
                (nyr_inst_t){.op = NYIR_INLINE_ASM,
                             .dst = -1,
                             .a = selector,
                             .b = -1,
                             .c = -1,
                             .imm = -78,
                             .symbol = "lkgs",
                             .effects = NYIR_EFFECT_VOLATILE |
                                        NYIR_EFFECT_MAY_TRAP |
                                        NYIR_EFFECT_CONTROL},
                NULL);
  }

  static const char clear_pages_old_call[] = "call __clear_pages_unrolled";
  static const char clear_pages_stosq[] = "rep stosq";
  static const char clear_pages_stosb[] = "rep stosb";
  static const char clear_user_call[] = "call rep_stos_alternative";
  bool clear_user_alternative =
      !asm_goto && INLINE_ASM_HAS("# ALT: oldinstr") &&
      INLINE_ASM_HAS(clear_user_call) && INLINE_ASM_HAS(clear_pages_stosb);
  bool clear_pages_alternative =
      !asm_goto && INLINE_ASM_HAS("# ALT: oldinstr") &&
      INLINE_ASM_HAS(clear_pages_old_call) &&
      INLINE_ASM_HAS(clear_pages_stosq) && INLINE_ASM_HAS(clear_pages_stosb);
  if (clear_pages_alternative || clear_user_alternative) {
    ny_ctok_t output_names[3];
    static const char *const output_constraints[] = {"+c", "+D", "+r"};
    if (!expect(p, ":"))
      return defer_unsupported_asm(p, open, assembly);
    for (size_t output = 0; output < 3; ++output) {
      size_t constraint_end = p->pos;
      if ((output && !expect(p, ",")) || !parse_inline_asm_operand_name(p) ||
          !inline_asm_string_sequence_is(p, output_constraints[output],
                                         &constraint_end))
        return defer_unsupported_asm(p, open, assembly);
      p->pos = constraint_end;
      if (!expect(p, "(") || !parse_name(p, &output_names[output]) ||
          !expect(p, ")"))
        return defer_unsupported_asm(p, open, assembly);
    }
    c_local_t *length_local = find_local(p, output_names[0]);
    c_local_t *address_local = find_local(p, output_names[1]);
    if (!length_local || !address_local || length_local->array_rank ||
        address_local->array_rank || length_local->is_const ||
        address_local->is_const || !c_type_is_integer(length_local->type) ||
        !c_type_is_pointer(address_local->type) || !expect(p, ":") ||
        !inline_asm_string_sequence_is(p, "a", &p->pos) || !expect(p, "("))
      return defer_unsupported_asm(p, open, assembly);
    c_value_t zero = parse_expr(p, 1);
    if (zero.value < 0 || !c_type_is_integer(zero.type) || !expect(p, ")"))
      return defer_unsupported_asm(p, open, assembly);
    if (accept(p, ":")) {
      size_t clobber_end = p->pos;
      if (!inline_asm_clobber_list_is(p, &clobber_end))
        return defer_unsupported_asm(p, open, assembly);
      p->pos = clobber_end;
    }
    if (!expect(p, ")") || !expect(p, ";"))
      return defer_unsupported_asm(p, open, assembly);
    int address = lower_local_load(p, address_local);
    int length = lower_local_load(p, length_local);
    free(assembly);
    if (address < 0 || length < 0)
      return false;
    nyr_inst_t call = {
        .op = NYIR_CALL,
        .dst = -1,
        .a = address,
        .b = clear_user_alternative ? length : zero.value,
        .c = clear_user_alternative ? -1 : length,
        .d = -1,
        .e = -1,
        .f = -1,
        .imm = clear_user_alternative ? 2 : 3,
        .flags = NYIR_INST_F_EXTERN,
        .effects = NYIR_EFFECT_CALL | NYIR_EFFECT_READ_MEMORY |
                   NYIR_EFFECT_WRITE_MEMORY | NYIR_EFFECT_VOLATILE,
        .scalar_class = NYIR_SCALAR_I64_RAW,
        .symbol = clear_user_alternative ? "rep_stos_alternative" : "memset"};
    nyr_call_arg_class_set(call.arg_scalar_classes, 0, NYIR_SCALAR_I64_RAW);
    nyr_call_arg_class_set(call.arg_scalar_classes, 1, NYIR_SCALAR_I64_RAW);
    if (!clear_user_alternative)
      nyr_call_arg_class_set(call.arg_scalar_classes, 2, NYIR_SCALAR_I64_RAW);
    if (!clear_user_alternative)
      return emit(p, call, NULL);
    int remaining = emit_value(p, call);
    return remaining >= 0 && lower_local_store(p, length_local, remaining);
  }

  bool rdmsr_safe_native_template =
      !asm_goto && INLINE_ASM_HAS("1: rdmsr ; xor %[err],%[err]") &&
      INLINE_ASM_HAS(".macro extable_type_reg");
  if (rdmsr_safe_native_template && p->abi == NYC_ABI_I386) {
    ny_ctok_t error_name, value_name;
    if (!expect(p, ":") || !parse_named_inline_asm_operand(p, &error_name) ||
        !inline_asm_name_matches(error_name, (const unsigned char *)"err", 3) ||
        !inline_asm_string_sequence_is(p, "=r", &p->pos) || !expect(p, "(") ||
        !parse_name(p, &error_name) || !expect(p, ")") || !expect(p, ",") ||
        !inline_asm_string_sequence_is(p, "=A", &p->pos) || !expect(p, "(") ||
        !parse_name(p, &value_name) || !expect(p, ")") || !expect(p, ":") ||
        !inline_asm_string_sequence_is(p, "c", &p->pos) || !expect(p, "(")) {
      free(assembly);
      return false;
    }
    c_value_t msr = parse_expr(p, 1);
    c_local_t *error_local = find_local(p, error_name);
    c_local_t *value_local = find_local(p, value_name);
    if (msr.value < 0 || !c_type_is_integer(msr.type) || !expect(p, ")") ||
        !expect(p, ")") || !expect(p, ";") || !error_local || !value_local ||
        error_local == value_local || error_local->type != C_I32 ||
        (value_local->type != C_U64 && value_local->type != C_I64) ||
        error_local->array_rank || value_local->array_rank ||
        error_local->is_const || value_local->is_const)
      return defer_unsupported_asm(p, open, assembly);
    free(assembly);
    error_local->address_taken = true;
    value_local->address_taken = true;
    int error_address =
        emit_value(p, (nyr_inst_t){.op = NYIR_ADDR_LOCAL,
                                   .dst = -1,
                                   .imm = (int64_t)error_local->slot});
    int value_address =
        emit_value(p, (nyr_inst_t){.op = NYIR_ADDR_LOCAL,
                                   .dst = -1,
                                   .imm = (int64_t)value_local->slot});
    if (error_address < 0 || value_address < 0)
      return false;
    char slots[80];
    int slots_len =
        snprintf(slots, sizeof(slots), "rdmsr_safe_pair32;nyc_slots=%zu,%zu",
                 error_local->slot, value_local->slot);
    if (slots_len < 0 || (size_t)slots_len >= sizeof(slots)) {
      fail(p, "out of memory lowering paired safe rdmsr inline assembly");
      return false;
    }
    const char *lowered = nyr_func_own_symbol_copy(p->out, slots);
    if (!lowered) {
      fail(p, "out of memory lowering paired safe rdmsr inline assembly");
      return false;
    }
    return emit(p,
                (nyr_inst_t){
                    .op = NYIR_INLINE_ASM,
                    .dst = -1,
                    .a = msr.value,
                    .b = error_address,
                    .c = value_address,
                    .d = -1,
                    .e = -1,
                    .f = -1,
                    .imm = -30,
                    .symbol = lowered,
                    .effects = NYIR_EFFECT_READ_MEMORY |
                               NYIR_EFFECT_WRITE_MEMORY | NYIR_EFFECT_MAY_TRAP |
                               NYIR_EFFECT_CONTROL | NYIR_EFFECT_VOLATILE},
                NULL);
  }

  if (rdmsr_safe_native_template) {
    ny_ctok_t error_name, low_name, high_name;
    if (!expect(p, ":") || !parse_named_inline_asm_operand(p, &error_name) ||
        !inline_asm_name_matches(error_name, (const unsigned char *)"err", 3) ||
        !inline_asm_string_sequence_is(p, "=r", &p->pos) || !expect(p, "(") ||
        !parse_name(p, &error_name) || !expect(p, ")") || !expect(p, ",") ||
        !inline_asm_string_sequence_is(p, "=a", &p->pos) || !expect(p, "(") ||
        !parse_name(p, &low_name) || !expect(p, ")") || !expect(p, ",") ||
        !inline_asm_string_sequence_is(p, "=d", &p->pos) || !expect(p, "(") ||
        !parse_name(p, &high_name) || !expect(p, ")") || !expect(p, ":") ||
        !inline_asm_string_sequence_is(p, "c", &p->pos) || !expect(p, "(")) {
      free(assembly);
      return false;
    }
    c_value_t msr = parse_expr(p, 1);
    c_local_t *error_local = find_local(p, error_name);
    c_local_t *low_local = find_local(p, low_name);
    c_local_t *high_local = find_local(p, high_name);
    if (msr.value < 0 || !c_type_is_integer(msr.type) || !expect(p, ")") ||
        !expect(p, ")") || !expect(p, ";") || !error_local || !low_local ||
        !high_local || error_local == low_local || error_local == high_local ||
        low_local == high_local || error_local->type != C_I32 ||
        (low_local->type != C_U64 && low_local->type != C_I64) ||
        (high_local->type != C_U64 && high_local->type != C_I64) ||
        error_local->array_rank || low_local->array_rank ||
        high_local->array_rank || error_local->is_const ||
        low_local->is_const || high_local->is_const)
      return defer_unsupported_asm(p, open, assembly);
    free(assembly);
    error_local->address_taken = true;
    low_local->address_taken = true;
    high_local->address_taken = true;
    int error_address = emit_value(p, (nyr_inst_t){.op = NYIR_ADDR_LOCAL,
                                                   .dst = -1,
                                                   .imm = error_local->slot});
    int low_address = emit_value(
        p,
        (nyr_inst_t){.op = NYIR_ADDR_LOCAL, .dst = -1, .imm = low_local->slot});
    int high_address = emit_value(p, (nyr_inst_t){.op = NYIR_ADDR_LOCAL,
                                                  .dst = -1,
                                                  .imm = high_local->slot});
    if (error_address < 0 || low_address < 0 || high_address < 0)
      return false;
    char slots[80];
    int slots_len =
        snprintf(slots, sizeof(slots), "rdmsr_safe;nyc_slots=%zu,%zu,%zu",
                 error_local->slot, low_local->slot, high_local->slot);
    if (slots_len < 0 || (size_t)slots_len >= sizeof(slots)) {
      fail(p, "out of memory lowering safe rdmsr inline assembly");
      return false;
    }
    const char *lowered = nyr_func_own_symbol_copy(p->out, slots);
    if (!lowered) {
      fail(p, "out of memory lowering safe rdmsr inline assembly");
      return false;
    }
    return emit(p,
                (nyr_inst_t){
                    .op = NYIR_INLINE_ASM,
                    .dst = -1,
                    .a = msr.value,
                    .b = error_address,
                    .c = -1,
                    .d = low_address,
                    .e = high_address,
                    .f = -1,
                    .imm = -28,
                    .symbol = lowered,
                    .effects = NYIR_EFFECT_READ_MEMORY |
                               NYIR_EFFECT_WRITE_MEMORY | NYIR_EFFECT_MAY_TRAP |
                               NYIR_EFFECT_CONTROL | NYIR_EFFECT_VOLATILE},
                NULL);
  }

  bool rdmsr_template = INLINE_ASM_HAS("1: rdmsr") &&
                        INLINE_ASM_HAS(".pushsection __ex_table") &&
                        INLINE_ASM_HAS(".long 9");
  bool rdmsr_mce_template = INLINE_ASM_HAS("1: rdmsr") &&
                            INLINE_ASM_HAS(".pushsection __ex_table") &&
                            INLINE_ASM_HAS(".long 13");
  if (!asm_goto && p->abi == NYC_ABI_I386 && rdmsr_template) {
    ny_ctok_t value_name;
    if (!expect(p, ":") || !parse_inline_asm_operand_name(p) ||
        !inline_asm_string_sequence_is(p, "=A", &p->pos) || !expect(p, "(") ||
        !parse_name(p, &value_name) || !expect(p, ")") || !expect(p, ":") ||
        !inline_asm_string_sequence_is(p, "c", &p->pos) || !expect(p, "(")) {
      free(assembly);
      return false;
    }
    c_value_t msr = parse_expr(p, 1);
    c_local_t *value_local = find_local(p, value_name);
    if (msr.value < 0 || !c_type_is_integer(msr.type) || !expect(p, ")") ||
        !expect(p, ")") || !expect(p, ";") || !value_local ||
        (value_local->type != C_U64 && value_local->type != C_I64) ||
        value_local->is_const || value_local->array_rank) {
      fail(p, "unsupported i386 rdmsr output operands");
      free(assembly);
      return false;
    }
    free(assembly);
    value_local->address_taken = true;
    int address =
        emit_value(p, (nyr_inst_t){.op = NYIR_ADDR_LOCAL,
                                   .dst = -1,
                                   .imm = (int64_t)value_local->slot,
                                   .scalar_class = NYIR_SCALAR_I64_RAW});
    if (address < 0)
      return false;
    char slots[64];
    int slots_len = snprintf(slots, sizeof(slots), "rdmsr_pair;nyc_slot=%zu",
                             value_local->slot);
    if (slots_len < 0 || (size_t)slots_len >= sizeof(slots)) {
      fail(p, "out of memory lowering i386 rdmsr inline assembly");
      return false;
    }
    const char *symbol = nyr_func_own_symbol_copy(p->out, slots);
    if (!symbol) {
      fail(p, "out of memory lowering i386 rdmsr inline assembly");
      return false;
    }
    return emit(p,
                (nyr_inst_t){
                    .op = NYIR_INLINE_ASM,
                    .dst = -1,
                    .a = msr.value,
                    .b = -1,
                    .c = -1,
                    .d = address,
                    .e = address,
                    .imm = -27,
                    .symbol = symbol,
                    .effects = NYIR_EFFECT_CONTROL | NYIR_EFFECT_VOLATILE |
                               NYIR_EFFECT_READ_MEMORY |
                               NYIR_EFFECT_WRITE_MEMORY | NYIR_EFFECT_MAY_TRAP},
                NULL);
  }
  if (!asm_goto && (rdmsr_template || rdmsr_mce_template)) {
    ny_ctok_t low_name, high_name;
    if (!expect(p, ":") || !inline_asm_string_sequence_is(p, "=a", &p->pos) ||
        !expect(p, "(") || !parse_name(p, &low_name) || !expect(p, ")") ||
        !expect(p, ",") || !inline_asm_string_sequence_is(p, "=d", &p->pos) ||
        !expect(p, "(") || !parse_name(p, &high_name) || !expect(p, ")") ||
        !expect(p, ":") || !inline_asm_string_sequence_is(p, "c", &p->pos) ||
        !expect(p, "(")) {
      free(assembly);
      return false;
    }
    c_value_t msr = parse_expr(p, 1);
    c_local_t *low_local = find_local(p, low_name);
    c_local_t *high_local = find_local(p, high_name);
    if (msr.value < 0 || !c_type_is_integer(msr.type) || !expect(p, ")") ||
        !expect(p, ")") || !expect(p, ";") || !low_local || !high_local ||
        low_local == high_local ||
        (low_local->type != C_U32 && low_local->type != C_I32) ||
        (high_local->type != C_U32 && high_local->type != C_I32) ||
        low_local->is_const || high_local->is_const) {
      free(assembly);
      return false;
    }
    free(assembly);
    low_local->address_taken = true;
    high_local->address_taken = true;
    int low_address = emit_value(
        p,
        (nyr_inst_t){.op = NYIR_ADDR_LOCAL, .dst = -1, .imm = low_local->slot});
    int high_address = emit_value(p, (nyr_inst_t){.op = NYIR_ADDR_LOCAL,
                                                  .dst = -1,
                                                  .imm = high_local->slot});
    if (low_address < 0 || high_address < 0)
      return false;
    char slots[64];
    int slots_len = snprintf(slots, sizeof(slots), "%s;nyc_slots=%zu,%zu",
                             rdmsr_mce_template ? "rdmsr_mce" : "rdmsr",
                             low_local->slot, high_local->slot);
    if (slots_len < 0 || (size_t)slots_len >= sizeof(slots)) {
      fail(p, "out of memory lowering rdmsr inline assembly");
      return false;
    }
    const char *lowered_template = nyr_func_own_symbol_copy(p->out, slots);
    if (!lowered_template) {
      fail(p, "out of memory lowering rdmsr inline assembly");
      return false;
    }
    return emit(p,
                (nyr_inst_t){
                    .op = NYIR_INLINE_ASM,
                    .dst = -1,
                    .a = msr.value,
                    .b = -1,
                    .c = -1,
                    .d = low_address,
                    .e = high_address,
                    .imm = -27,
                    .symbol = lowered_template,
                    .effects = NYIR_EFFECT_READ_MEMORY |
                               NYIR_EFFECT_WRITE_MEMORY | NYIR_EFFECT_MAY_TRAP |
                               NYIR_EFFECT_CONTROL | NYIR_EFFECT_VOLATILE},
                NULL);
  }

  if (asm_goto) {
    static const struct {
      const char *instruction;
      const char *output_constraint;
      const char *kind;
      size_t width;
    } faultable_loads[] = {{"movb %[umem],%[output]", "=q", "b", 1},
                           {"movw %[umem],%[output]", "=r", "w", 2},
                           {"movl %[umem],%[output]", "=r", "l", 4},
                           {"movq %[umem],%[output]", "=r", "q", 8}};
    int faultable_load_width = -1;
    if (memmem(assembly, assembly_len, ".pushsection __ex_table",
               sizeof(".pushsection __ex_table") - 1) &&
        memmem(assembly, assembly_len, "%l2", sizeof("%l2") - 1)) {
      for (size_t i = 0; i < sizeof(faultable_loads) / sizeof(*faultable_loads);
           ++i) {
        size_t instruction_len = strlen(faultable_loads[i].instruction);
        for (size_t at = 0; at + instruction_len <= assembly_len; ++at) {
          if (memcmp(assembly + at, faultable_loads[i].instruction,
                     instruction_len) == 0) {
            faultable_load_width = (int)i;
            break;
          }
        }
        if (faultable_load_width >= 0)
          break;
      }
    }
    if (faultable_load_width >= 0) {
      ny_ctok_t output_operand, input_operand, target_name;
      size_t constraint_end = p->pos;
      if (!expect(p, ":") ||
          !parse_named_inline_asm_operand(p, &output_operand) ||
          !inline_asm_name_matches(output_operand,
                                   (const unsigned char *)"output", 6) ||
          !inline_asm_string_sequence_is(
              p, faultable_loads[faultable_load_width].output_constraint,
              &constraint_end))
        return defer_unsupported_asm(p, open, assembly);
      p->pos = constraint_end;
      if (!expect(p, "("))
        return defer_unsupported_asm(p, open, assembly);
      c_value_t output_value = parse_expr(p, 1);
      const c_lvalue_t *output_lvalue =
          output_value.value >= 0 ? find_lvalue(p, output_value.value) : NULL;
      c_local_t *output_local =
          output_lvalue && output_lvalue->is_local &&
                  output_lvalue->local_slot >= 0
              ? find_local_slot(p, (size_t)output_lvalue->local_slot)
              : NULL;
      if (!output_lvalue || !expect(p, ")") || !expect(p, ":") ||
          !parse_named_inline_asm_operand(p, &input_operand) ||
          !inline_asm_name_matches(input_operand, (const unsigned char *)"umem",
                                   4) ||
          !inline_asm_string_sequence_is(p, "m", &p->pos) || !expect(p, "("))
        return defer_unsupported_asm(p, open, assembly);
      c_value_t memory = parse_expr(p, 1);
      const c_lvalue_t *memory_lvalue =
          memory.value >= 0 ? find_lvalue(p, memory.value) : NULL;
      int memory_address = memory.type == C_AGG ? memory.value
                           : memory_lvalue      ? memory_lvalue->address
                                                : -1;
      size_t width = faultable_loads[faultable_load_width].width;
      bool output_is_memory = !output_lvalue->is_local &&
                              output_lvalue->address >= 0 &&
                              !output_lvalue->bitfield;
      bool output_writable = output_lvalue->is_local
                                 ? output_local && !output_local->is_const &&
                                       !output_local->array_rank
                                 : output_is_memory;
      bool compatible_type = output_writable &&
                             (memory_lvalue || memory.type == C_AGG) &&
                             (c_type_is_integer(output_value.type) ||
                              c_type_is_pointer(output_value.type)) &&
                             c_type_size(p->abi, output_value.type) == width;
      if (!compatible_type || memory_address < 0 || !expect(p, ")") ||
          !expect(p, ":")) {
        return defer_unsupported_asm(p, open, assembly);
      }
      {
        size_t clobber_end = p->pos;
        if (inline_asm_clobber_list_is(p, &clobber_end))
          p->pos = clobber_end;
      }
      if (!expect(p, ":") || !parse_name(p, &target_name)) {
        return defer_unsupported_asm(p, open, assembly);
      }
      c_label_t *target = find_label_at(p, target_name, p->pos);
      if (!target || accept(p, ",") || !expect(p, ")") || !expect(p, ";"))
        return defer_unsupported_asm(p, open, assembly);
      int edge = lower_cleanup_edge(p, target);
      if (edge < 0) {
        free(assembly);
        return false;
      }
      int loaded = emit_value(
          p, (nyr_inst_t){
                 .op = NYIR_INLINE_ASM,
                 .dst = -1,
                 .a = memory_address,
                 .c = edge,
                 .imm = -83,
                 .effects = NYIR_EFFECT_READ_MEMORY | NYIR_EFFECT_MAY_TRAP |
                            NYIR_EFFECT_CONTROL | NYIR_EFFECT_VOLATILE,
                 .symbol = faultable_loads[faultable_load_width].kind});
      if (loaded >= 0 && c_type_is_integer(output_value.type))
        loaded = normalize_integer(p, output_value.type, loaded);
      bool ok = loaded >= 0;
      if (ok && output_local) {
        ok = lower_local_store(p, output_local, loaded);
      } else if (ok && output_is_memory) {
        bool is_volatile = output_lvalue->is_volatile;
        if (width == 2)
          ok = lower_store_u16_field(p, output_lvalue->address, loaded,
                                     output_value.type, is_volatile);
        else
          ok = emit(
              p,
              (nyr_inst_t){.op = NYIR_STORE_I64,
                           .dst = -1,
                           .a = output_lvalue->address,
                           .c = loaded,
                           .effects = NYIR_EFFECT_WRITE_MEMORY |
                                      NYIR_EFFECT_MAY_TRAP |
                                      (is_volatile ? NYIR_EFFECT_VOLATILE : 0),
                           .flags = c_local_memory_flags(p, output_value.type),
                           .scalar_class = NYIR_SCALAR_I64_RAW},
              NULL);
      } else {
        ok = false;
      }
      free(assembly);
      return ok;
    }
    static const struct {
      const char *instruction;
      const char *register_name;
      const char *kind;
    } faultable_stores[] = {{"movb %0,%1", "%r11b", "b"},
                            {"movw %0,%1", "%r11w", "w"},
                            {"movl %0,%1", "%r11d", "l"},
                            {"movq %0,%1", "%r11", "q"}};
    int faultable_store_width = -1;
    for (size_t i = 0;
         i < sizeof(faultable_stores) / sizeof(faultable_stores[0]); ++i) {
      size_t instruction_len = strlen(faultable_stores[i].instruction);
      bool has_instruction = false;
      for (size_t at = 0; at + instruction_len <= assembly_len; ++at)
        if (memcmp(assembly + at, faultable_stores[i].instruction,
                   instruction_len) == 0) {
          has_instruction = true;
          break;
        }
      if (has_instruction &&
          memmem(assembly, assembly_len, ".pushsection __ex_table",
                 sizeof(".pushsection __ex_table") - 1) &&
          memmem(assembly, assembly_len, "%l2", sizeof("%l2") - 1) &&
          memmem(assembly, assembly_len, ".long 3", sizeof(".long 3") - 1)) {
        faultable_store_width = (int)i;
        break;
      }
    }
    if (faultable_store_width >= 0) {
      if (!expect(p, ":") || !expect(p, ":"))
        return defer_unsupported_asm(p, open, assembly);
      size_t constraint_end = p->pos;
      if (!inline_asm_string_sequence_is(p, "iq", &constraint_end) &&
          !inline_asm_string_sequence_is(p, "ir", &constraint_end) &&
          !inline_asm_string_sequence_is(p, "er", &constraint_end))
        return defer_unsupported_asm(p, open, assembly);
      p->pos = constraint_end;
      if (!expect(p, "("))
        return defer_unsupported_asm(p, open, assembly);
      c_parser_t constant_probe = *p;
      int64_t immediate = 0;
      bool has_immediate = parse_global_const_expr(
          &constant_probe, &constant_probe.pos, &immediate);
      c_value_t input = parse_expr(p, 1);
      int source = input.value;
      if (source < 0 && has_immediate && c_type_is_integer(input.type))
        source = emit_i64_constant(p, immediate);
      if (source < 0 ||
          (!c_type_is_integer(input.type) && !c_type_is_pointer(input.type)) ||
          !expect(p, ")") || !expect(p, ","))
        return defer_unsupported_asm(p, open, assembly);
      constraint_end = p->pos;
      if (!inline_asm_string_sequence_is(p, "m", &constraint_end))
        return defer_unsupported_asm(p, open, assembly);
      p->pos = constraint_end;
      if (!expect(p, "("))
        return defer_unsupported_asm(p, open, assembly);
      c_value_t memory = parse_expr(p, 1);
      const c_lvalue_t *memory_lvalue =
          memory.value >= 0 ? find_lvalue(p, memory.value) : NULL;
      static const c_type_t widths[] = {C_CHAR, C_I16, C_I32, C_I64};
      static const c_type_t unsigned_widths[] = {C_U8, C_U16, C_U32, C_U64};
      bool wrapper_memory = memory_lvalue && memory_lvalue->address < 0 &&
                            c_type_is_pointer(memory_lvalue->type);
      bool matching_width =
          memory_lvalue &&
          (wrapper_memory || memory_lvalue->type == C_AGG ||
           (faultable_store_width == 3 &&
            c_type_is_pointer(memory_lvalue->type)) ||
           memory_lvalue->type == widths[faultable_store_width] ||
           memory_lvalue->type == unsigned_widths[faultable_store_width]);
      int memory_address =
          wrapper_memory || (memory_lvalue && memory_lvalue->type == C_AGG &&
                             memory_lvalue->address < 0)
              ? memory.value
          : memory_lvalue ? memory_lvalue->address
                          : (memory.type == C_AGG ? memory.value : -1);
      if (memory_address < 0 && memory.value >= 0 &&
          c_type_is_pointer(memory.type))
        memory_address = memory.value;
      bool ok_mem = memory_address >= 0 &&
                    (matching_width || memory_lvalue || memory.type == C_AGG);
      if (!ok_mem || !expect(p, ")") || !expect(p, ":"))
        return defer_unsupported_asm(p, open, assembly);
      {
        size_t clobber_end = p->pos;
        if (inline_asm_clobber_list_is(p, &clobber_end))
          p->pos = clobber_end;
        if (!expect(p, ":"))
          return defer_unsupported_asm(p, open, assembly);
      }
      ny_ctok_t target_name;
      if (!parse_name(p, &target_name))
        return defer_unsupported_asm(p, open, assembly);
      c_label_t *target = find_label_at(p, target_name, p->pos);
      if (!target || accept(p, ",") || !expect(p, ")") || !expect(p, ";"))
        return defer_unsupported_asm(p, open, assembly);
      free(assembly);
      int edge = lower_cleanup_edge(p, target);
      return edge >= 0 &&
             emit(p,
                  (nyr_inst_t){
                      .op = NYIR_INLINE_ASM,
                      .dst = -1,
                      .a = source,
                      .b = memory_address,
                      .c = edge,
                      .imm = -82,
                      .effects = NYIR_EFFECT_READ_MEMORY |
                                 NYIR_EFFECT_WRITE_MEMORY |
                                 NYIR_EFFECT_MAY_TRAP | NYIR_EFFECT_CONTROL |
                                 NYIR_EFFECT_VOLATILE,
                      .symbol = faultable_stores[faultable_store_width].kind},
                  NULL);
    }
    static const char *const static_cpu_has_needles[] = {
        "# ALT: oldinstr", "testb %[bitnum], %a[cap_byte]", "jnz %l[t_yes]",
        "jmp %l[t_no]"};
    bool static_cpu_has = true;
    for (size_t needle = 0; needle < sizeof(static_cpu_has_needles) /
                                         sizeof(static_cpu_has_needles[0]);
         ++needle) {
      size_t needle_len = strlen(static_cpu_has_needles[needle]);
      bool found = false;
      for (size_t i = 0; i + needle_len <= assembly_len; ++i)
        if (memcmp(assembly + i, static_cpu_has_needles[needle], needle_len) ==
            0) {
          found = true;
          break;
        }
      static_cpu_has &= found;
    }
    if (static_cpu_has) {
      if (!expect(p, ":") || !expect(p, ":")) {
        free(assembly);
        return false;
      }
      int inputs[3] = {-1, -1, -1};
      for (size_t i = 0; i < 3; ++i) {
        if (i && !expect(p, ",")) {
          free(assembly);
          return false;
        }
        ny_ctok_t name;
        size_t constraint_end = p->pos;
        if (!parse_named_inline_asm_operand(p, &name) ||
            name.kind != NY_CTOK_IDENT ||
            !inline_asm_string_sequence_is(p, "i", &constraint_end)) {
          return defer_unsupported_asm(p, open, assembly);
        }
        p->pos = constraint_end;
        if (!expect(p, "(")) {
          free(assembly);
          return false;
        }
        c_value_t input = parse_expr(p, 1);
        if (input.value < 0 || !expect(p, ")") ||
            (i == 2 ? !c_type_is_pointer(input.type)
                    : !c_type_is_integer(input.type))) {
          return defer_unsupported_asm(p, open, assembly);
        }
        inputs[i] = input.value;
      }
      if (!expect(p, ":") || !expect(p, ":")) {
        free(assembly);
        return false;
      }
      ny_ctok_t target_names[2];
      c_label_t *targets[2] = {NULL, NULL};
      for (size_t i = 0; i < 2; ++i) {
        if ((i && !expect(p, ",")) || !parse_name(p, &target_names[i])) {
          free(assembly);
          return false;
        }
        targets[i] = find_label_at(p, target_names[i], p->pos);
        if (!targets[i]) {
          return defer_unsupported_asm(p, open, assembly);
        }
      }
      if (!expect(p, ")") || !expect(p, ";")) {
        free(assembly);
        return false;
      }
      free(assembly);
      int loaded =
          emit_value(p, (nyr_inst_t){.op = NYIR_LOAD_I64,
                                     .dst = -1,
                                     .a = inputs[2],
                                     .effects = NYIR_EFFECT_READ_MEMORY |
                                                NYIR_EFFECT_MAY_TRAP |
                                                NYIR_EFFECT_VOLATILE,
                                     .flags = c_local_memory_flags(p, C_U8),
                                     .scalar_class = NYIR_SCALAR_I64_RAW});
      int byte = loaded < 0 ? loaded : normalize_integer(p, C_U8, loaded);
      int masked =
          byte < 0 ? byte : emit_i64_binary(p, NYIR_AND_I64, byte, inputs[1]);
      int zero = masked < 0 ? masked : emit_i64_constant(p, 0);
      int present =
          zero < 0 ? zero : emit_i64_compare(p, masked, zero, NYIR_CMP_NE);
      int yes = present >= 0 ? lower_cleanup_edge(p, targets[0]) : -1;
      int no = yes >= 0 ? lower_cleanup_edge(p, targets[1]) : -1;
      if (no < 0 || !emit_branch_if(p, present, yes) || !emit_branch(p, no))
        return false;
      return true;
    }
    static const char *const required[] = {
        "jmp %l[l_yes]", ".pushsection __jump_table", "%c0 + %c1"};
    for (size_t r = 0; r < sizeof(required) / sizeof(required[0]); ++r) {
      size_t needle_len = strlen(required[r]);
      bool found = false;
      for (size_t i = 0; i + needle_len <= assembly_len; ++i)
        if (memcmp(assembly + i, required[r], needle_len) == 0) {
          found = true;
          break;
        }
      if (!found)
        return defer_unsupported_asm(p, open, assembly);
    }
    if (!expect(p, ":") || !expect(p, ":"))
      return defer_unsupported_asm(p, open, assembly);
    int inputs[2] = {-1, -1};
    for (size_t i = 0; i < 2; ++i) {
      if (i && !expect(p, ","))
        return defer_unsupported_asm(p, open, assembly);
      size_t constraint_end = p->pos;
      if (!parse_inline_asm_operand_name(p) ||
          !inline_asm_string_sequence_is(p, "i", &constraint_end))
        return defer_unsupported_asm(p, open, assembly);
      p->pos = constraint_end;
      if (!expect(p, "("))
        return defer_unsupported_asm(p, open, assembly);
      c_value_t input = parse_expr(p, 1);
      if (input.value < 0 || !expect(p, ")") ||
          (i == 0 ? !c_type_is_pointer(input.type)
                  : !c_type_is_integer(input.type)))
        return defer_unsupported_asm(p, open, assembly);
      inputs[i] = input.value;
    }
    if (!expect(p, ":") || !expect(p, ":"))
      return defer_unsupported_asm(p, open, assembly);
    ny_ctok_t target_name;
    if (!parse_name(p, &target_name))
      return defer_unsupported_asm(p, open, assembly);
    c_label_t *target = find_label_at(p, target_name, p->pos);
    if (!target || accept(p, ",") || !expect(p, ")") || !expect(p, ";"))
      return defer_unsupported_asm(p, open, assembly);
    int edge = lower_cleanup_edge(p, target);
    if (edge < 0) {
      free(assembly);
      return false;
    }
    if (!append_asm_bytes(&assembly, &assembly_len, &assembly_cap, "\0", 1)) {
      free(assembly);
      return false;
    }
    bool ok = emit(p,
                   (nyr_inst_t){.op = NYIR_INLINE_ASM,
                                .dst = -1,
                                .a = inputs[0],
                                .b = inputs[1],
                                .c = edge,
                                .imm = -14,
                                .symbol = (const char *)assembly},
                   NULL);
    if (!ok)
      free(assembly);
    return ok;
  }

  if (!asm_goto && assembly_len &&
      memmem(assembly, assembly_len, ".pushsection __ex_table",
             sizeof(".pushsection __ex_table") - 1)) {
    static const struct {
      const char *instruction;
      const char *kind;
      size_t width;
    } register_loads[] = {{"movb %[umem],%[output]", "b", 1},
                          {"movw %[umem],%[output]", "w", 2},
                          {"movl %[umem],%[output]", "l", 4},
                          {"movq %[umem],%[output]", "q", 8}};
    int width_index = -1;
    for (size_t i = 0; i < sizeof(register_loads) / sizeof(*register_loads);
         ++i) {
      size_t instruction_len = strlen(register_loads[i].instruction);
      for (size_t at = 0; at + instruction_len <= assembly_len; ++at) {
        if (memcmp(assembly + at, register_loads[i].instruction,
                   instruction_len) == 0) {
          width_index = (int)i;
          break;
        }
      }
      if (width_index >= 0)
        break;
    }
    if (width_index >= 0) {
      ny_ctok_t error_name, memory_name;
      size_t constraint_end = p->pos;
      if (!expect(p, ":") || !parse_named_inline_asm_operand(p, &error_name) ||
          !inline_asm_name_matches(error_name, (const unsigned char *)"errout",
                                   6) ||
          !inline_asm_string_sequence_is(p, "=r", &constraint_end))
        return defer_unsupported_asm(p, open, assembly);
      p->pos = constraint_end;
      if (!expect(p, "(") || !parse_name(p, &error_name) || !expect(p, ")") ||
          !expect(p, ",") || !parse_named_inline_asm_operand(p, &memory_name) ||
          !inline_asm_name_matches(memory_name, (const unsigned char *)"output",
                                   6) ||
          !inline_asm_string_sequence_is(p, "=a", &p->pos) || !expect(p, "("))
        return defer_unsupported_asm(p, open, assembly);
      c_value_t output_value = parse_expr(p, 1);
      const c_lvalue_t *output_lvalue =
          output_value.value >= 0 ? find_lvalue(p, output_value.value) : NULL;
      if (output_value.value < 0 || !output_lvalue ||
          !c_type_is_integer(output_value.type) || !expect(p, ")") ||
          !expect(p, ":") || !parse_named_inline_asm_operand(p, &memory_name) ||
          !inline_asm_name_matches(memory_name, (const unsigned char *)"umem",
                                   4) ||
          !inline_asm_string_sequence_is(p, "m", &p->pos) || !expect(p, "("))
        return defer_unsupported_asm(p, open, assembly);
      c_local_t *error_output = find_local(p, error_name);
      c_value_t memory = parse_expr(p, 1);
      const c_lvalue_t *memory_lvalue =
          memory.value >= 0 ? find_lvalue(p, memory.value) : NULL;
      int memory_address = memory.type == C_AGG ? memory.value
                           : memory_lvalue      ? memory_lvalue->address
                                                : -1;
      c_local_t *output_local =
          output_lvalue->is_local
              ? find_local_slot(p, (size_t)output_lvalue->local_slot)
              : NULL;
      if (!error_output || error_output->is_const ||
          output_lvalue->is_volatile ||
          (output_lvalue->is_local &&
           (!output_local || output_local->is_const)) ||
          error_output->array_rank || !c_type_is_integer(error_output->type) ||
          memory_address < 0 || !expect(p, ")") || !expect(p, ",") ||
          !inline_asm_string_sequence_is(p, "0", &p->pos) || !expect(p, "("))
        return defer_unsupported_asm(p, open, assembly);
      c_value_t error_input = parse_expr(p, 1);
      const c_lvalue_t *error_input_lvalue =
          error_input.value >= 0 ? find_lvalue(p, error_input.value) : NULL;
      if (error_input.value < 0 || !error_input_lvalue ||
          !error_input_lvalue->is_local ||
          error_input_lvalue->local_slot != (int)error_output->slot ||
          error_input.type != error_output->type || !expect(p, ")") ||
          !expect(p, ")") || !expect(p, ";")) {
        return defer_unsupported_asm(p, open, assembly);
      }
      error_output->address_taken = true;
      int error_address =
          emit_value(p, (nyr_inst_t){.op = NYIR_ADDR_LOCAL,
                                     .dst = -1,
                                     .imm = (int64_t)error_output->slot});
      if (error_address < 0) {
        free(assembly);
        return false;
      }
      char marker[64];
      int marker_len =
          snprintf(marker, sizeof(marker), "%s;nyc_slot=%zu",
                   register_loads[width_index].kind, error_output->slot);
      const char *symbol = marker_len > 0 && (size_t)marker_len < sizeof(marker)
                               ? nyr_func_own_symbol_copy(p->out, marker)
                               : NULL;
      if (!symbol) {
        free(assembly);
        fail(p, "out of memory lowering faultable user-memory load");
        return false;
      }
      int loaded = emit_value(
          p, (nyr_inst_t){
                 .op = NYIR_INLINE_ASM,
                 .dst = -1,
                 .a = memory_address,
                 .b = error_input.value,
                 .c = error_address,
                 .imm = -84,
                 .effects = NYIR_EFFECT_READ_MEMORY | NYIR_EFFECT_WRITE_MEMORY |
                            NYIR_EFFECT_MAY_TRAP | NYIR_EFFECT_VOLATILE,
                 .symbol = symbol});
      if (loaded >= 0)
        loaded = normalize_integer(p, output_value.type, loaded);
      int output_address = output_lvalue->address;
      if (output_address < 0 && output_local)
        output_address =
            emit_value(p, (nyr_inst_t){.op = NYIR_ADDR_LOCAL,
                                       .dst = -1,
                                       .imm = (int64_t)output_local->slot});
      bool stored =
          output_local ? lower_local_store(p, output_local, loaded)
          : output_value.type == C_U8 || output_value.type == C_CHAR
              ? lower_store_u8_field(p, output_address, loaded, false)
          : output_value.type == C_I16 || output_value.type == C_U16
              ? lower_store_u16_field(p, output_address, loaded,
                                      output_value.type, false)
          : output_value.type == C_I32 || output_value.type == C_U32
              ? lower_store_i32_field(p, output_address, loaded, false)
              : lower_store_field(p, output_address, loaded, false, false);
      bool ok = loaded >= 0 && output_address >= 0 && stored;
      free(assembly);
      return ok;
    }
  }

  static const char div0_alt_operation[] = "div %2";
  static const char div0_alt_feature[] = ".4byte (22*32 + (1*32+ 1))";
  bool div0_alternative = INLINE_ASM_HAS("# ALT: oldinstr") &&
                          INLINE_ASM_HAS("# ALT: replacement") &&
                          INLINE_ASM_HAS(div0_alt_operation) &&
                          INLINE_ASM_HAS(div0_alt_feature);
  if (div0_alternative) {
    if (!expect(p, ":") || !expect(p, ":"))
      return defer_unsupported_asm(p, open, assembly);
    const char *const div0_constraints[] = {"a", "d", "r"};
    const int64_t div0_inputs[] = {0, 0, 1};
    for (size_t i = 0; i < 3; ++i) {
      if (i && !expect(p, ","))
        return defer_unsupported_asm(p, open, assembly);
      if (!parse_inline_asm_operand_name(p))
        return defer_unsupported_asm(p, open, assembly);
      size_t constraint_end = p->pos;
      if (!inline_asm_string_sequence_is(p, div0_constraints[i],
                                         &constraint_end))
        return defer_unsupported_asm(p, open, assembly);
      p->pos = constraint_end;
      if (!expect(p, "("))
        return defer_unsupported_asm(p, open, assembly);
      int64_t value = 0;
      bool previous_asm_constant_context = p->inline_asm_constant_context;
      p->inline_asm_constant_context = true;
      bool constant = parse_global_const_expr(p, &p->pos, &value);
      p->inline_asm_constant_context = previous_asm_constant_context;
      if (!constant || value != div0_inputs[i] || !expect(p, ")"))
        return defer_unsupported_asm(p, open, assembly);
    }
    if (!expect(p, ")") || !expect(p, ";"))
      return defer_unsupported_asm(p, open, assembly);
    c_global_t fallback_boot_cpu = {0};
    c_agg_field_t fallback_capability = {0};
    c_global_t *boot_cpu = NULL;
    const c_agg_field_t *capability = NULL;
    find_boot_cpu_capability(p, &fallback_boot_cpu, &fallback_capability,
                             &boot_cpu, &capability);
    if (!capability || capability->size <= 737 / 8 ||
        boot_cpu->has_asm_register) {
      free(assembly);
      fail(p, "DIV0 alternative requires boot_cpu_data.x86_capability");
      return false;
    }
    static const char data_prefix[] = "__nyc_data_";
    static const char extern_prefix[] = "__nyc_extern_";
    const char *symbol_prefix = boot_cpu->is_external || boot_cpu->is_weak
                                    ? extern_prefix
                                    : data_prefix;
    size_t symbol_prefix_len = strlen(symbol_prefix);
    size_t symbol_len = symbol_prefix_len + boot_cpu->name.len;
    char *symbol = malloc(symbol_len + 1);
    if (!symbol) {
      free(assembly);
      fail(p, "out of memory lowering DIV0 alternative");
      return false;
    }
    memcpy(symbol, symbol_prefix, symbol_prefix_len);
    memcpy(symbol + symbol_prefix_len, boot_cpu->name.start,
           boot_cpu->name.len);
    symbol[symbol_len] = '\0';
    int base = emit_value(
        p, (nyr_inst_t){.op = NYIR_ADDR_SYMBOL, .dst = -1, .symbol = symbol});
    free(symbol);
    size_t byte_offset = capability->offset + 737 / 8;
    int displacement = emit_i64_constant(p, (int64_t)byte_offset);
    int address = base < 0 || displacement < 0
                      ? -1
                      : emit_i64_binary(p, NYIR_ADD_I64, base, displacement);
    int byte =
        address < 0
            ? -1
            : emit_value(p, (nyr_inst_t){.op = NYIR_LOAD_I64,
                                         .dst = -1,
                                         .a = address,
                                         .effects = NYIR_EFFECT_READ_MEMORY |
                                                    NYIR_EFFECT_MAY_TRAP |
                                                    NYIR_EFFECT_VOLATILE,
                                         .flags = c_local_memory_flags(p, C_U8),
                                         .scalar_class = NYIR_SCALAR_I64_RAW});
    int mask = byte < 0 ? -1 : emit_i64_constant(p, 1u << (737 & 7));
    int masked = mask < 0 ? -1 : emit_i64_binary(p, NYIR_AND_I64, byte, mask);
    int zero = masked < 0 ? -1 : emit_i64_constant(p, 0);
    int enabled =
        zero < 0 ? -1 : emit_i64_compare(p, masked, zero, NYIR_CMP_NE);
    if (enabled < 0 || p->next_label > INT_MAX - 2) {
      free(assembly);
      return false;
    }
    const char *div0_assembly =
        "pushq %rax\n\tpushq %rdx\n\tpushq %rcx\n\t"
        "movl $0, %eax\n\tmovl $0, %edx\n\tmovl $1, %ecx\n\t"
        "divl %ecx\n\tpopq %rcx\n\tpopq %rdx\n\tpopq %rax";
    const char *owned_assembly =
        nyr_func_own_symbol_copy(p->out, div0_assembly);
    if (!owned_assembly) {
      free(assembly);
      fail(p, "out of memory lowering DIV0 alternative");
      return false;
    }
    size_t div_label = (size_t)p->next_label++;
    size_t join_label = (size_t)p->next_label++;
    bool ok = emit_branch_if(p, enabled, (int64_t)div_label) &&
              emit_branch(p, (int64_t)join_label) &&
              emit_label(p, (int64_t)div_label) &&
              emit(p,
                   (nyr_inst_t){.op = NYIR_INLINE_ASM,
                                .dst = -1,
                                .a = -1,
                                .b = -1,
                                .c = -1,
                                .d = -1,
                                .symbol = owned_assembly},
                   NULL) &&
              emit_label(p, (int64_t)join_label);
    free(assembly);
    return ok;
  }

  static const char ibpb_alt_call[] = "call write_ibpb";
  static const char ibpb_alt_feature[] = ".4byte ( 7*32+26)";
  bool ibpb_alternative = INLINE_ASM_HAS("# ALT: oldinstr") &&
                          INLINE_ASM_HAS("# ALT: replacement") &&
                          INLINE_ASM_HAS(ibpb_alt_call) &&
                          INLINE_ASM_HAS(ibpb_alt_feature);
  if (ibpb_alternative) {
    if (!expect(p, ":") || !parse_inline_asm_operand_name(p))
      return defer_unsupported_asm(p, open, assembly);
    size_t output_constraint_end = p->pos;
    if (!inline_asm_string_sequence_is(p, "+r", &output_constraint_end))
      return defer_unsupported_asm(p, open, assembly);
    p->pos = output_constraint_end;
    if (!expect(p, "(") || !tok_is(p, p->pos, "current_stack_pointer"))
      return defer_unsupported_asm(p, open, assembly);
    ++p->pos;
    if (!expect(p, ")") || !expect(p, ":") || !expect(p, ":"))
      return defer_unsupported_asm(p, open, assembly);
    static const char *const ibpb_clobbers[] = {"rax", "rcx", "rdx", "memory"};
    for (size_t i = 0; i < sizeof(ibpb_clobbers) / sizeof(ibpb_clobbers[0]);
         ++i) {
      if (i && !expect(p, ","))
        return defer_unsupported_asm(p, open, assembly);
      size_t clobber_end = p->pos;
      if (!inline_asm_string_sequence_is(p, ibpb_clobbers[i], &clobber_end))
        return defer_unsupported_asm(p, open, assembly);
      p->pos = clobber_end;
    }
    if (!expect(p, ")") || !expect(p, ";"))
      return defer_unsupported_asm(p, open, assembly);
    c_global_t fallback_boot_cpu = {0};
    c_agg_field_t fallback_capability = {0};
    c_global_t *boot_cpu = NULL;
    const c_agg_field_t *capability = NULL;
    find_boot_cpu_capability(p, &fallback_boot_cpu, &fallback_capability,
                             &boot_cpu, &capability);
    if (!capability || capability->size <= 250 / 8 ||
        boot_cpu->has_asm_register) {
      free(assembly);
      fail(p, "IBPB alternative requires boot_cpu_data.x86_capability");
      return false;
    }
    static const char data_prefix[] = "__nyc_data_";
    static const char extern_prefix[] = "__nyc_extern_";
    const char *symbol_prefix = boot_cpu->is_external || boot_cpu->is_weak
                                    ? extern_prefix
                                    : data_prefix;
    size_t symbol_prefix_len = strlen(symbol_prefix);
    size_t symbol_len = symbol_prefix_len + boot_cpu->name.len;
    char *symbol = malloc(symbol_len + 1);
    if (!symbol) {
      free(assembly);
      fail(p, "out of memory lowering IBPB alternative");
      return false;
    }
    memcpy(symbol, symbol_prefix, symbol_prefix_len);
    memcpy(symbol + symbol_prefix_len, boot_cpu->name.start,
           boot_cpu->name.len);
    symbol[symbol_len] = '\0';
    int base = emit_value(
        p, (nyr_inst_t){.op = NYIR_ADDR_SYMBOL, .dst = -1, .symbol = symbol});
    free(symbol);
    size_t byte_offset = capability->offset + 250 / 8;
    int displacement = emit_i64_constant(p, (int64_t)byte_offset);
    int address = base < 0 || displacement < 0
                      ? -1
                      : emit_i64_binary(p, NYIR_ADD_I64, base, displacement);
    int byte =
        address < 0
            ? -1
            : emit_value(p, (nyr_inst_t){.op = NYIR_LOAD_I64,
                                         .dst = -1,
                                         .a = address,
                                         .effects = NYIR_EFFECT_READ_MEMORY |
                                                    NYIR_EFFECT_MAY_TRAP |
                                                    NYIR_EFFECT_VOLATILE,
                                         .flags = c_local_memory_flags(p, C_U8),
                                         .scalar_class = NYIR_SCALAR_I64_RAW});
    int mask = byte < 0 ? -1 : emit_i64_constant(p, 1u << (250 & 7));
    int masked = mask < 0 ? -1 : emit_i64_binary(p, NYIR_AND_I64, byte, mask);
    int zero = masked < 0 ? -1 : emit_i64_constant(p, 0);
    int present =
        zero < 0 ? -1 : emit_i64_compare(p, masked, zero, NYIR_CMP_NE);
    if (present < 0 || p->next_label > INT_MAX - 2) {
      free(assembly);
      return false;
    }
    size_t call_label = (size_t)p->next_label++;
    size_t join_label = (size_t)p->next_label++;
    bool ok = emit_branch_if(p, present, (int64_t)call_label) &&
              emit_branch(p, (int64_t)join_label) &&
              emit_label(p, (int64_t)call_label) &&
              emit(p,
                   (nyr_inst_t){.op = NYIR_CALL,
                                .dst = -1,
                                .imm = 0,
                                .flags = NYIR_INST_F_EXTERN,
                                .effects = NYIR_EFFECT_CALL,
                                .symbol = "write_ibpb"},
                   NULL) &&
              emit_label(p, (int64_t)join_label);
    free(assembly);
    return ok;
  }

  static const char task_size_alt_old[] = "movq %[small],%0";
  static const char task_size_alt_new[] = "movq %[large],%0";
  static const char task_size_alt_feature[] = ".4byte (16*32+16)";
  bool task_size_alternative = INLINE_ASM_HAS("# ALT: oldinstr") &&
                               INLINE_ASM_HAS(task_size_alt_old) &&
                               INLINE_ASM_HAS("# ALT: replacement") &&
                               INLINE_ASM_HAS(task_size_alt_new) &&
                               INLINE_ASM_HAS(task_size_alt_feature);
  if (task_size_alternative) {
    if (!expect(p, ":") || !parse_inline_asm_operand_name(p) ||
        !inline_asm_string_sequence_is(p, "=r", &p->pos) || !expect(p, "("))
      return defer_unsupported_asm(p, open, assembly);
    ny_ctok_t output_name;
    if (!parse_name(p, &output_name) || !expect(p, ")") || !expect(p, ":"))
      return defer_unsupported_asm(p, open, assembly);
    c_local_t *output = find_local(p, output_name);
    if (!output || output->type != C_U64 || output->is_const ||
        output->array_rank)
      return defer_unsupported_asm(p, open, assembly);
    int64_t selected_sizes[2] = {0, 0};
    for (size_t input_index = 0; input_index < 3; ++input_index) {
      if (input_index && !expect(p, ","))
        return defer_unsupported_asm(p, open, assembly);
      ny_ctok_t input_name;
      if (!parse_named_inline_asm_operand(p, &input_name))
        return defer_unsupported_asm(p, open, assembly);
      if ((input_index == 1 &&
           !inline_asm_name_matches(input_name, (const unsigned char *)"small",
                                    5)) ||
          (input_index == 2 &&
           !inline_asm_name_matches(input_name, (const unsigned char *)"large",
                                    5)))
        return defer_unsupported_asm(p, open, assembly);
      size_t constraint_end = p->pos;
      if (!inline_asm_string_sequence_is(p, "i", &constraint_end))
        return defer_unsupported_asm(p, open, assembly);
      p->pos = constraint_end;
      if (!expect(p, "("))
        return defer_unsupported_asm(p, open, assembly);
      int64_t value = 0;
      bool previous_asm_constant_context = p->inline_asm_constant_context;
      p->inline_asm_constant_context = true;
      bool is_constant = parse_global_const_expr(p, &p->pos, &value);
      p->inline_asm_constant_context = previous_asm_constant_context;
      if (!is_constant || !expect(p, ")"))
        return defer_unsupported_asm(p, open, assembly);
      if (input_index)
        selected_sizes[input_index - 1] = value;
    }
    if (!expect(p, ")") || !expect(p, ";"))
      return defer_unsupported_asm(p, open, assembly);
    c_global_t fallback_boot_cpu = {0};
    c_agg_field_t fallback_capability = {0};
    c_global_t *boot_cpu = NULL;
    const c_agg_field_t *capability = NULL;
    find_boot_cpu_capability(p, &fallback_boot_cpu, &fallback_capability,
                             &boot_cpu, &capability);
    if (!capability || capability->size <= 528 / 8 ||
        boot_cpu->has_asm_register) {
      free(assembly);
      fail(p, "LA57 alternative requires boot_cpu_data.x86_capability");
      return false;
    }
    static const char data_prefix[] = "__nyc_data_";
    static const char extern_prefix[] = "__nyc_extern_";
    const char *symbol_prefix = boot_cpu->is_external || boot_cpu->is_weak
                                    ? extern_prefix
                                    : data_prefix;
    size_t symbol_prefix_len = strlen(symbol_prefix);
    size_t symbol_len = symbol_prefix_len + boot_cpu->name.len;
    char *symbol = malloc(symbol_len + 1);
    if (!symbol) {
      free(assembly);
      fail(p, "out of memory lowering LA57 alternative");
      return false;
    }
    memcpy(symbol, symbol_prefix, symbol_prefix_len);
    memcpy(symbol + symbol_prefix_len, boot_cpu->name.start,
           boot_cpu->name.len);
    symbol[symbol_len] = '\0';
    int base = emit_value(
        p, (nyr_inst_t){.op = NYIR_ADDR_SYMBOL, .dst = -1, .symbol = symbol});
    free(symbol);
    size_t byte_offset = capability->offset + 528 / 8;
    int displacement = emit_i64_constant(p, (int64_t)byte_offset);
    int address = base < 0 || displacement < 0
                      ? -1
                      : emit_i64_binary(p, NYIR_ADD_I64, base, displacement);
    int byte =
        address < 0
            ? -1
            : emit_value(p, (nyr_inst_t){.op = NYIR_LOAD_I64,
                                         .dst = -1,
                                         .a = address,
                                         .effects = NYIR_EFFECT_READ_MEMORY |
                                                    NYIR_EFFECT_MAY_TRAP |
                                                    NYIR_EFFECT_VOLATILE,
                                         .flags = c_local_memory_flags(p, C_U8),
                                         .scalar_class = NYIR_SCALAR_I64_RAW});
    int mask = byte < 0 ? -1 : emit_i64_constant(p, 1);
    int present = mask < 0 ? -1 : emit_i64_binary(p, NYIR_AND_I64, byte, mask);
    int zero = present < 0 ? -1 : emit_i64_constant(p, 0);
    present = zero < 0 ? -1 : emit_i64_compare(p, present, zero, NYIR_CMP_NE);
    if (present < 0 || p->next_label > INT_MAX - 3) {
      free(assembly);
      return false;
    }
    size_t large_label = (size_t)p->next_label++;
    size_t join_label = (size_t)p->next_label++;
    bool ok = emit_branch_if(p, present, (int64_t)large_label);
    int small_value = emit_i64_constant(p, selected_sizes[0]);
    ok = ok && small_value >= 0 && lower_local_store(p, output, small_value) &&
         emit_branch(p, (int64_t)join_label) &&
         emit_label(p, (int64_t)large_label);
    int large_value = ok ? emit_i64_constant(p, selected_sizes[1]) : -1;
    ok = ok && large_value >= 0 && lower_local_store(p, output, large_value) &&
         emit_label(p, (int64_t)join_label);
    free(assembly);
    return ok;
  }

  static const char index_mask_asm[] = "cmp %1,%2; sbb %0,%0";
  if (assembly_len == sizeof(index_mask_asm) - 1 &&
      memcmp(assembly, index_mask_asm, assembly_len) == 0 && accept(p, ":")) {
    size_t constraint_end = p->pos;
    ny_ctok_t output_name;
    if (!inline_asm_string_sequence_is(p, "=r", &constraint_end))
      return defer_unsupported_asm(p, open, assembly);
    p->pos = constraint_end;
    if (!expect(p, "(") || !parse_name(p, &output_name) || !expect(p, ")") ||
        !expect(p, ":"))
      return defer_unsupported_asm(p, open, assembly);
    c_local_t *output = find_local(p, output_name);
    if (!output || output->array_rank || output->is_const ||
        !c_type_is_integer(output->type))
      return defer_unsupported_asm(p, open, assembly);

    constraint_end = p->pos;
    if (!inline_asm_string_sequence_is(p, "g", &constraint_end))
      return defer_unsupported_asm(p, open, assembly);
    p->pos = constraint_end;
    if (!expect(p, "("))
      return defer_unsupported_asm(p, open, assembly);
    c_value_t size = parse_expr(p, 1);
    if (size.value < 0 || !c_type_is_integer(size.type) || !expect(p, ")") ||
        !expect(p, ","))
      return defer_unsupported_asm(p, open, assembly);
    constraint_end = p->pos;
    if (!inline_asm_string_sequence_is(p, "r", &constraint_end))
      return defer_unsupported_asm(p, open, assembly);
    p->pos = constraint_end;
    if (!expect(p, "("))
      return defer_unsupported_asm(p, open, assembly);
    c_value_t index = parse_expr(p, 1);
    if (index.value < 0 || !c_type_is_integer(index.type) || !expect(p, ")") ||
        !expect(p, ":"))
      return defer_unsupported_asm(p, open, assembly);
    size_t clobber_end = p->pos;
    if (!inline_asm_clobber_list_is(p, &clobber_end))
      return defer_unsupported_asm(p, open, assembly);
    p->pos = clobber_end;
    if (!expect(p, ")"))
      return defer_unsupported_asm(p, open, assembly);

    int in_bounds = emit_u64_compare(p, index.value, size.value, NYIR_CMP_LT);
    int zero = in_bounds < 0 ? -1 : emit_i64_constant(p, 0);
    int mask =
        zero < 0 ? -1 : emit_i64_binary(p, NYIR_SUB_I64, zero, in_bounds);
    free(assembly);
    return mask >= 0 && lower_local_store(p, output, mask);
  }

  static const char lar_valid_ss_asm[] =
      "lar %[old_ss], %[ar]\n\tjz 1f\n\txorl %[ar], %[ar]\n\t1:";
  if (!asm_goto && assembly_len == sizeof(lar_valid_ss_asm) - 1 &&
      memcmp(assembly, lar_valid_ss_asm, assembly_len) == 0 && accept(p, ":")) {
    ny_ctok_t output_operand, output_name, input_operand;
    if (!parse_named_inline_asm_operand(p, &output_operand) ||
        !inline_asm_name_matches(output_operand, (const unsigned char *)"ar",
                                 2) ||
        !inline_asm_string_sequence_is(p, "=r", &p->pos) || !expect(p, "(") ||
        !parse_name(p, &output_name) || !expect(p, ")") || !expect(p, ":") ||
        !parse_named_inline_asm_operand(p, &input_operand) ||
        !inline_asm_name_matches(input_operand, (const unsigned char *)"old_ss",
                                 6) ||
        !inline_asm_string_sequence_is(p, "rm", &p->pos) || !expect(p, "(")) {
      free(assembly);
      return false;
    }
    c_value_t input = parse_expr(p, 1);
    c_local_t *output = find_local(p, output_name);
    if (input.value < 0 || !c_type_is_integer(input.type) || !expect(p, ")") ||
        !expect(p, ")") || !expect(p, ";") || !output || output->array_rank ||
        output->is_const || (output->type != C_U32 && output->type != C_I32)) {
      fail(p, "unsupported x86-64 LAR validity operands");
      free(assembly);
      return false;
    }
    int result = emit_value(
        p, (nyr_inst_t){.op = NYIR_INLINE_ASM,
                        .dst = -1,
                        .a = input.value,
                        .imm = -110,
                        .symbol = "lar-valid-ss",
                        .scalar_class = NYIR_SCALAR_I64_RAW,
                        .effects = NYIR_EFFECT_CONTROL | NYIR_EFFECT_VOLATILE});
    free(assembly);
    return result >= 0 && lower_local_store(p, output, result);
  }

  static const char rdtsc_asm[] = "rdtsc";
  static const char rdpkru_asm[] = "rdpkru";
  bool rdpkru_plain = assembly_len == sizeof(rdpkru_asm) - 1 &&
                      memcmp(assembly, rdpkru_asm, assembly_len) == 0;
  if (!asm_goto && rdpkru_plain && accept(p, ":")) {
    ny_ctok_t pkru_operand, edx_operand, ecx_operand;
    ny_ctok_t pkru_name, edx_name, ecx_name;
    if (!parse_named_inline_asm_operand(p, &pkru_operand) ||
        !inline_asm_string_sequence_is(p, "=a", &p->pos) || !expect(p, "(") ||
        !parse_name(p, &pkru_name) || !expect(p, ")") || !expect(p, ",") ||
        !parse_named_inline_asm_operand(p, &edx_operand) ||
        !inline_asm_string_sequence_is(p, "=d", &p->pos) || !expect(p, "(") ||
        !parse_name(p, &edx_name) || !expect(p, ")") || !expect(p, ":") ||
        !parse_named_inline_asm_operand(p, &ecx_operand) ||
        !inline_asm_string_sequence_is(p, "c", &p->pos) || !expect(p, "(") ||
        !parse_name(p, &ecx_name) || !expect(p, ")") || !expect(p, ")") ||
        !expect(p, ";")) {
      free(assembly);
      return false;
    }
    c_local_t *pkru = find_local(p, pkru_name);
    c_local_t *edx = find_local(p, edx_name);
    c_local_t *ecx = find_local(p, ecx_name);
    if (!pkru || !edx || !ecx || pkru == edx || pkru->is_const ||
        edx->is_const || ecx->array_rank || pkru->array_rank ||
        edx->array_rank || (pkru->type != C_U32 && pkru->type != C_I32) ||
        (edx->type != C_U32 && edx->type != C_I32) ||
        (ecx->type != C_U32 && ecx->type != C_I32)) {
      fail(p, "unsupported x86-64 rdpkru operands");
      free(assembly);
      return false;
    }
    int ecx_value = lower_local_load(p, ecx);
    int pkru_value =
        ecx_value < 0
            ? -1
            : emit_value(p, (nyr_inst_t){.op = NYIR_INLINE_ASM,
                                         .dst = -1,
                                         .a = ecx_value,
                                         .imm = -107,
                                         .symbol = "rdpkru",
                                         .effects = NYIR_EFFECT_CONTROL |
                                                    NYIR_EFFECT_VOLATILE});
    int zero = pkru_value < 0 ? -1 : emit_i64_constant(p, 0);
    free(assembly);
    return pkru_value >= 0 && zero >= 0 &&
           lower_local_store(p, pkru, pkru_value) &&
           lower_local_store(p, edx, zero);
  }
  static const char wrfsbase_asm[] = "wrfsbase %0";
  static const char wrgsbase_asm[] = "wrgsbase %0";
  bool wrfsbase_plain = assembly_len == sizeof(wrfsbase_asm) - 1 &&
                        memcmp(assembly, wrfsbase_asm, assembly_len) == 0;
  bool wrgsbase_plain = assembly_len == sizeof(wrgsbase_asm) - 1 &&
                        memcmp(assembly, wrgsbase_asm, assembly_len) == 0;
  if (!asm_goto && (wrfsbase_plain || wrgsbase_plain) && accept(p, ":") &&
      expect(p, ":")) {
    ny_ctok_t input_operand, input_name;
    if (!parse_named_inline_asm_operand(p, &input_operand) ||
        !inline_asm_string_sequence_is(p, "r", &p->pos) || !expect(p, "(") ||
        !parse_name(p, &input_name) || !expect(p, ")") || !expect(p, ":")) {
      free(assembly);
      return false;
    }
    size_t clobber_end = p->pos;
    if (!inline_asm_clobber_list_is(p, &clobber_end)) {
      free(assembly);
      return false;
    }
    p->pos = clobber_end;
    if (!expect(p, ")") || !expect(p, ";")) {
      free(assembly);
      return false;
    }
    c_local_t *input = find_local(p, input_name);
    if (!input || input->array_rank || !c_type_is_integer(input->type)) {
      fail(p, "unsupported x86-64 wrfsbase input");
      free(assembly);
      return false;
    }
    int value = lower_local_load(p, input);
    free(assembly);
    return value >= 0 &&
           emit(p,
                (nyr_inst_t){.op = NYIR_INLINE_ASM,
                             .dst = -1,
                             .a = value,
                             .imm = -109,
                             .symbol = wrfsbase_plain ? "wrfsbase" : "wrgsbase",
                             .effects = NYIR_EFFECT_CONTROL |
                                        NYIR_EFFECT_VOLATILE |
                                        NYIR_EFFECT_WRITE_MEMORY},
                NULL);
  }
  bool futex_atomic_xchg_asm =
      assembly_len &&
      memmem(assembly, assembly_len, "xchgl %0, %2",
             sizeof("xchgl %0, %2") - 1) &&
      memmem(assembly, assembly_len, ".pushsection __ex_table",
             sizeof(".pushsection __ex_table") - 1);
  bool futex_atomic_xadd_asm =
      assembly_len &&
      memmem(assembly, assembly_len, "xaddl %0, %2",
             sizeof("xaddl %0, %2") - 1) &&
      memmem(assembly, assembly_len, ".pushsection __ex_table",
             sizeof(".pushsection __ex_table") - 1);
  if ((futex_atomic_xchg_asm || futex_atomic_xadd_asm) && accept(p, ":")) {
    if (!inline_asm_string_sequence_is(p, "=r", &p->pos) || !expect(p, "("))
      return defer_unsupported_asm(p, open, assembly);
    ny_ctok_t oldval_name, ret_name;
    if (!parse_name(p, &oldval_name) || !expect(p, ")") || !expect(p, ",") ||
        !inline_asm_string_sequence_is(p, "=r", &p->pos) || !expect(p, "(") ||
        !parse_name(p, &ret_name) || !expect(p, ")") || !expect(p, ",") ||
        !inline_asm_string_sequence_is(p, "+m", &p->pos) || !expect(p, "("))
      return defer_unsupported_asm(p, open, assembly);
    c_value_t memory = parse_expr(p, 1);
    const c_lvalue_t *memory_lvalue =
        memory.value >= 0 ? find_lvalue(p, memory.value) : NULL;
    if (!memory_lvalue || memory_lvalue->address < 0 ||
        (memory_lvalue->type != C_I32 && memory_lvalue->type != C_U32) ||
        !expect(p, ")") || !expect(p, ":") ||
        !inline_asm_string_sequence_is(p, "0", &p->pos) || !expect(p, "("))
      return defer_unsupported_asm(p, open, assembly);
    c_value_t replacement = parse_expr(p, 1);
    if (replacement.value < 0 ||
        (replacement.type != C_I32 && replacement.type != C_U32) ||
        !expect(p, ")") || !expect(p, ",") ||
        !inline_asm_string_sequence_is(p, "1", &p->pos) || !expect(p, "("))
      return defer_unsupported_asm(p, open, assembly);
    int64_t ret_initial = -1;
    if (!parse_global_const_expr(p, &p->pos, &ret_initial) ||
        ret_initial != 0 || !expect(p, ")"))
      return defer_unsupported_asm(p, open, assembly);
    if (accept(p, ":") && !inline_asm_string_sequence_is(p, "memory", &p->pos))
      return defer_unsupported_asm(p, open, assembly);
    if (!expect(p, ")") || !expect(p, ";"))
      return defer_unsupported_asm(p, open, assembly);
    c_local_t *oldval = find_local(p, oldval_name);
    c_local_t *ret = find_local(p, ret_name);
    if (!oldval || !ret || oldval == ret || oldval->array_rank ||
        ret->array_rank || oldval->is_const || ret->is_const ||
        (oldval->type != C_I32 && oldval->type != C_U32) ||
        (ret->type != C_I32 && ret->type != C_U32))
      return defer_unsupported_asm(p, open, assembly);
    int ret_input = emit_i64_constant(p, 0);
    int ret_address = emit_value(p, (nyr_inst_t){.op = NYIR_ADDR_LOCAL,
                                                 .dst = -1,
                                                 .imm = (int64_t)ret->slot});
    if (ret_input < 0 || ret_address < 0) {
      free(assembly);
      return false;
    }
    oldval->address_taken = true;
    ret->address_taken = true;
    int old_value = emit_value(
        p, (nyr_inst_t){
               .op = NYIR_INLINE_ASM,
               .dst = -1,
               .a = memory_lvalue->address,
               .b = replacement.value,
               .c = ret_input,
               .d = ret_address,
               .imm = futex_atomic_xchg_asm ? -164 : -165,
               .effects = NYIR_EFFECT_READ_MEMORY | NYIR_EFFECT_WRITE_MEMORY |
                          NYIR_EFFECT_MAY_TRAP | NYIR_EFFECT_VOLATILE,
               .symbol = futex_atomic_xchg_asm ? "futex_xchg32_extable"
                                               : "futex_xadd32_extable"});
    if (old_value < 0 || !lower_local_store(p, oldval, old_value)) {
      free(assembly);
      return false;
    }
    free(assembly);
    return true;
  }
  bool futex_atomic_op2_asm =
      assembly_len &&
      memmem(assembly, assembly_len, "movl", sizeof("movl") - 1) &&
      memmem(assembly, assembly_len, "%2, %0", sizeof("%2, %0") - 1) &&
      memmem(assembly, assembly_len, "%0, %3", sizeof("%0, %3") - 1) &&
      memmem(assembly, assembly_len, "cmpxchgl %3, %2",
             sizeof("cmpxchgl %3, %2") - 1) &&
      memmem(assembly, assembly_len, ".pushsection __ex_table",
             sizeof(".pushsection __ex_table") - 1);
  const char *futex_op2 = NULL;
  if (futex_atomic_op2_asm) {
    if (memmem(assembly, assembly_len, "xorl %4, %3",
               sizeof("xorl %4, %3") - 1))
      futex_op2 = "xor";
    else if (memmem(assembly, assembly_len, "andl %4, %3",
                    sizeof("andl %4, %3") - 1))
      futex_op2 = "and";
    else if (memmem(assembly, assembly_len, "orl %4, %3",
                    sizeof("orl %4, %3") - 1))
      futex_op2 = "or";
  }
  if (futex_op2 && accept(p, ":")) {
    if (!inline_asm_string_sequence_is(p, "=&a", &p->pos) || !expect(p, "("))
      return defer_unsupported_asm(p, open, assembly);
    ny_ctok_t oldval_name, ret_name, tem_name;
    if (!parse_name(p, &oldval_name) || !expect(p, ")") || !expect(p, ",") ||
        !inline_asm_string_sequence_is(p, "=&r", &p->pos) || !expect(p, "(") ||
        !parse_name(p, &ret_name) || !expect(p, ")") || !expect(p, ",") ||
        !inline_asm_string_sequence_is(p, "+m", &p->pos) || !expect(p, "("))
      return defer_unsupported_asm(p, open, assembly);
    c_value_t memory = parse_expr(p, 1);
    const c_lvalue_t *memory_lvalue =
        memory.value >= 0 ? find_lvalue(p, memory.value) : NULL;
    if (!memory_lvalue || memory_lvalue->address < 0 ||
        (memory_lvalue->type != C_I32 && memory_lvalue->type != C_U32) ||
        !expect(p, ")") || !expect(p, ",") ||
        !inline_asm_string_sequence_is(p, "=&r", &p->pos) || !expect(p, "(") ||
        !parse_name(p, &tem_name) || !expect(p, ")") || !expect(p, ":") ||
        !inline_asm_string_sequence_is(p, "r", &p->pos) || !expect(p, "("))
      return defer_unsupported_asm(p, open, assembly);
    c_value_t operand = parse_expr(p, 1);
    if (operand.value < 0 || (operand.type != C_I32 && operand.type != C_U32) ||
        !expect(p, ")") || !expect(p, ",") ||
        !inline_asm_string_sequence_is(p, "1", &p->pos) || !expect(p, "("))
      return defer_unsupported_asm(p, open, assembly);
    int64_t ret_initial = -1;
    if (!parse_global_const_expr(p, &p->pos, &ret_initial) ||
        ret_initial != 0 || !expect(p, ")") || !expect(p, ")") ||
        !expect(p, ";"))
      return defer_unsupported_asm(p, open, assembly);
    c_local_t *oldval = find_local(p, oldval_name);
    c_local_t *ret = find_local(p, ret_name);
    c_local_t *tem = find_local(p, tem_name);
    if (!oldval || !ret || !tem || oldval == ret || oldval == tem ||
        ret == tem || oldval->array_rank || ret->array_rank ||
        tem->array_rank || oldval->is_const || ret->is_const || tem->is_const ||
        (oldval->type != C_I32 && oldval->type != C_U32) ||
        (ret->type != C_I32 && ret->type != C_U32) ||
        (tem->type != C_I32 && tem->type != C_U32))
      return defer_unsupported_asm(p, open, assembly);
    int ret_input = emit_i64_constant(p, 0);
    int ret_address = emit_value(p, (nyr_inst_t){.op = NYIR_ADDR_LOCAL,
                                                 .dst = -1,
                                                 .imm = (int64_t)ret->slot});
    if (ret_input < 0 || ret_address < 0) {
      free(assembly);
      return false;
    }
    ret->address_taken = true;
    int old_value = emit_value(
        p, (nyr_inst_t){.op = NYIR_INLINE_ASM,
                        .dst = -1,
                        .a = memory_lvalue->address,
                        .b = operand.value,
                        .c = ret_input,
                        .d = ret_address,
                        .imm = -166,
                        .effects = NYIR_EFFECT_READ_MEMORY |
                                   NYIR_EFFECT_WRITE_MEMORY |
                                   NYIR_EFFECT_MAY_TRAP | NYIR_EFFECT_VOLATILE,
                        .symbol = futex_op2});
    if (old_value < 0 || !lower_local_store(p, oldval, old_value)) {
      free(assembly);
      return false;
    }
    free(assembly);
    return true;
  }
  bool futex_runtime_mask_asm =
      assembly_len &&
      memmem(assembly, assembly_len, "and $0x12345678, %k0",
             sizeof("and $0x12345678, %k0") - 1) &&
      memmem(assembly, assembly_len, ".long 1b - 4 - .",
             sizeof(".long 1b - 4 - .") - 1);
  if (futex_runtime_mask_asm && accept(p, ":")) {
    if (!inline_asm_string_sequence_is(p, "+r", &p->pos) || !expect(p, "("))
      return defer_unsupported_asm(p, open, assembly);
    ny_ctok_t output_name;
    if (!parse_name(p, &output_name) || !expect(p, ")") || !expect(p, ")") ||
        !expect(p, ";"))
      return defer_unsupported_asm(p, open, assembly);
    c_local_t *output = find_local(p, output_name);
    if (!output || output->array_rank || output->is_const ||
        (output->type != C_I32 && output->type != C_U32))
      return defer_unsupported_asm(p, open, assembly);
    int input = lower_local_load(p, output);
    int result =
        input < 0 ? -1
                  : emit_value(
                        p, (nyr_inst_t){.op = NYIR_INLINE_ASM,
                                        .dst = -1,
                                        .a = input,
                                        .imm = -163,
                                        .effects = NYIR_EFFECT_VOLATILE,
                                        .symbol = "runtime_mask___futex_mask"});
    bool ok = result >= 0 && lower_local_store(p, output, result);
    free(assembly);
    return ok;
  }
  static const char futex_cmpxchg_asm[] = "lock cmpxchgl";
  bool futex_atomic_cmpxchg_asm =
      assembly_len &&
      memmem(assembly, assembly_len, "lock cmpxchgl %3, %2",
             sizeof("lock cmpxchgl %3, %2") - 1) &&
      memmem(assembly, assembly_len, ".pushsection __ex_table",
             sizeof(".pushsection __ex_table") - 1);
  if (futex_atomic_cmpxchg_asm && accept(p, ":")) {
    if (!inline_asm_string_sequence_is(p, "+r", &p->pos) || !expect(p, "("))
      return defer_unsupported_asm(p, open, assembly);
    ny_ctok_t ret_name, oldval_name;
    if (!parse_name(p, &ret_name) || !expect(p, ")") || !expect(p, ",") ||
        !inline_asm_string_sequence_is(p, "=a", &p->pos) || !expect(p, "(") ||
        !parse_name(p, &oldval_name) || !expect(p, ")") || !expect(p, ",") ||
        !inline_asm_string_sequence_is(p, "+m", &p->pos) || !expect(p, "("))
      return defer_unsupported_asm(p, open, assembly);
    c_value_t memory = parse_expr(p, 1);
    const c_lvalue_t *memory_lvalue =
        memory.value >= 0 ? find_lvalue(p, memory.value) : NULL;
    if (!memory_lvalue || memory_lvalue->address < 0 ||
        (memory_lvalue->type != C_I32 && memory_lvalue->type != C_U32) ||
        !expect(p, ")") || !expect(p, ":") ||
        !inline_asm_string_sequence_is(p, "r", &p->pos) || !expect(p, "("))
      return defer_unsupported_asm(p, open, assembly);
    c_value_t replacement = parse_expr(p, 1);
    if (replacement.value < 0 ||
        (replacement.type != C_I32 && replacement.type != C_U32) ||
        !expect(p, ")") || !expect(p, ",") ||
        !inline_asm_string_sequence_is(p, "1", &p->pos) || !expect(p, "("))
      return defer_unsupported_asm(p, open, assembly);
    c_value_t expected_input = parse_expr(p, 1);
    const c_lvalue_t *expected_lvalue =
        expected_input.value >= 0 ? find_lvalue(p, expected_input.value) : NULL;
    if (!expected_lvalue || !expected_lvalue->is_local || !expect(p, ")") ||
        !expect(p, ":") ||
        !inline_asm_string_sequence_is(p, "memory", &p->pos) ||
        !expect(p, ")") || !expect(p, ";"))
      return defer_unsupported_asm(p, open, assembly);
    c_local_t *ret = find_local(p, ret_name);
    c_local_t *oldval = find_local(p, oldval_name);
    c_local_t *expected_local =
        find_local_slot(p, (size_t)expected_lvalue->local_slot);
    if (!ret || !oldval || ret == oldval || ret->array_rank ||
        oldval->array_rank || ret->is_const || oldval->is_const ||
        (ret->type != C_I32 && ret->type != C_U32) ||
        (oldval->type != C_I32 && oldval->type != C_U32) ||
        expected_local != oldval)
      return defer_unsupported_asm(p, open, assembly);
    int expected = lower_local_load(p, oldval);
    int ret_input = lower_local_load(p, ret);
    int ret_address = emit_value(p, (nyr_inst_t){.op = NYIR_ADDR_LOCAL,
                                                 .dst = -1,
                                                 .imm = (int64_t)ret->slot});
    if (expected < 0 || ret_input < 0 || ret_address < 0) {
      free(assembly);
      return false;
    }
    oldval->address_taken = true;
    ret->address_taken = true;
    int old_value = emit_value(
        p, (nyr_inst_t){.op = NYIR_INLINE_ASM,
                        .dst = -1,
                        .a = memory_lvalue->address,
                        .b = expected,
                        .c = replacement.value,
                        .d = ret_input,
                        .e = ret_address,
                        .imm = -162,
                        .effects = NYIR_EFFECT_READ_MEMORY |
                                   NYIR_EFFECT_WRITE_MEMORY |
                                   NYIR_EFFECT_MAY_TRAP | NYIR_EFFECT_VOLATILE,
                        .symbol = "futex_cmpxchg32_extable"});
    if (old_value < 0 || !lower_local_store(p, oldval, old_value)) {
      free(assembly);
      return false;
    }
    free(assembly);
    return true;
  }
  bool futex_robust_unlock_asm =
      assembly_len &&
      memmem(assembly, assembly_len, futex_cmpxchg_asm,
             sizeof(futex_cmpxchg_asm) - 1) &&
      memmem(assembly, assembly_len, "jnz ", sizeof("jnz ") - 1) &&
      (memmem(assembly, assembly_len, "movq", sizeof("movq") - 1) ||
       memmem(assembly, assembly_len, "movl", sizeof("movl") - 1));
  if (futex_robust_unlock_asm && accept(p, ":")) {
    ny_ctok_t tid_operand, tid_name, lock_operand, pop_operand, zero_operand;
    if (!parse_named_inline_asm_operand(p, &tid_operand) ||
        !inline_asm_name_matches(tid_operand, (const unsigned char *)"tid",
                                 3) ||
        !inline_asm_string_sequence_is(p, "+&a", &p->pos) || !expect(p, "(") ||
        !parse_name(p, &tid_name) || !expect(p, ")") || !expect(p, ":") ||
        !parse_named_inline_asm_operand(p, &lock_operand) ||
        !inline_asm_name_matches(lock_operand, (const unsigned char *)"lock",
                                 4) ||
        !inline_asm_string_sequence_is(p, "D", &p->pos) || !expect(p, "(")) {
      fail(p, "invalid x86-64 futex cmpxchg operands");
      return false;
    }
    c_value_t lock = parse_expr(p, 1);
    if (lock.value < 0 || !c_type_is_pointer(lock.type) || !expect(p, ")") ||
        !expect(p, ",") || !parse_named_inline_asm_operand(p, &pop_operand) ||
        !inline_asm_name_matches(pop_operand, (const unsigned char *)"pop",
                                 3) ||
        !inline_asm_string_sequence_is(p, "d", &p->pos) || !expect(p, "(")) {
      fail(p, "invalid x86-64 futex pending-operation pointer");
      return false;
    }
    c_value_t pop = parse_expr(p, 1);
    if (pop.value < 0 || !c_type_is_pointer(pop.type) || !expect(p, ")") ||
        !expect(p, ",") || !parse_named_inline_asm_operand(p, &zero_operand) ||
        !inline_asm_name_matches(zero_operand, (const unsigned char *)"zero",
                                 4) ||
        !inline_asm_string_sequence_is(p, "r", &p->pos) || !expect(p, "(")) {
      fail(p, "invalid x86-64 futex zero operand");
      return false;
    }
    int64_t zero_input = -1;
    if (!parse_global_const_expr(p, &p->pos, &zero_input) || zero_input != 0 ||
        !expect(p, ")") || !expect(p, ":") ||
        !inline_asm_string_sequence_is(p, "memory", &p->pos) ||
        !expect(p, ")") || !expect(p, ";")) {
      fail(p, "invalid x86-64 futex asm ending");
      return false;
    }
    c_local_t *tid = find_local(p, tid_name);
    if (!tid || tid->array_rank || tid->is_const ||
        (tid->type != C_I32 && tid->type != C_U32) ||
        (lock.type != C_I32_PTR && lock.type != C_U32_PTR) ||
        (pop.type != C_I32_PTR && pop.type != C_U32_PTR &&
         pop.type != C_I64_PTR && pop.type != C_U64_PTR))
      return defer_unsupported_asm(p, open, assembly);
    int expected = lower_local_load(p, tid);
    int zero = emit_i64_constant(p, 0);
    int old =
        expected < 0 || zero < 0
            ? -1
            : emit_value(p,
                         (nyr_inst_t){.op = NYIR_INLINE_ASM,
                                      .dst = -1,
                                      .a = lock.value,
                                      .b = expected,
                                      .c = zero,
                                      .d = -1,
                                      .imm = -12,
                                      .symbol = "lock cmpxchgl %edx, (%rcx)"});
    int success = old < 0 || expected < 0
                      ? -1
                      : emit_i64_compare(p, old, expected, NYIR_CMP_EQ);
    size_t clear_label = (size_t)p->next_label++;
    size_t done_label = (size_t)p->next_label++;
    bool ok =
        old >= 0 && success >= 0 && lower_local_store(p, tid, old) &&
        emit_branch_if(p, success, (int64_t)clear_label) &&
        emit_branch(p, (int64_t)done_label) &&
        emit_label(p, (int64_t)clear_label) &&
        emit(p,
             (nyr_inst_t){
                 .op = NYIR_STORE_I64,
                 .dst = -1,
                 .a = pop.value,
                 .c = zero,
                 .flags = pop.type == C_I32_PTR || pop.type == C_U32_PTR
                              ? NYIR_INST_F_MEM_I32
                              : 0,
                 .effects = NYIR_EFFECT_WRITE_MEMORY | NYIR_EFFECT_MAY_TRAP},
             NULL) &&
        emit_label(p, (int64_t)done_label);
    free(assembly);
    return ok;
  }
  /*
   * The 32-bit VDSO syscall macro wraps int $0x80 in Linux's ALTERNATIVE
   * directive.  Its operands still describe the ordinary i386 syscall ABI;
   * accept only this exact output/input/clobber shape and lower the fallback
   * instruction.  Runtime patching of the optional __kernel_vsyscall fast
   * path is not available in NYC-generated assembly.
   */
  if (memmem(assembly, assembly_len, "int $0x80", sizeof("int $0x80") - 1) &&
      memmem(assembly, assembly_len, "__kernel_vsyscall",
             sizeof("__kernel_vsyscall") - 1) &&
      accept(p, ":")) {
    if (!inline_asm_string_sequence_is(p, "+a", &p->pos) || !expect(p, "(")) {
      fail(p, "unsupported i386 VDSO syscall output constraint");
      return false;
    }
    ny_ctok_t result_name;
    if (!parse_name(p, &result_name) || !expect(p, ")") || !expect(p, ":")) {
      fail(p, "invalid i386 VDSO syscall output syntax");
      return false;
    }
    c_local_t *result_local = find_local(p, result_name);
    if (!result_local || result_local->array_rank || result_local->is_const ||
        !c_type_is_integer(result_local->type)) {
      fail(p, "invalid i386 VDSO syscall result local");
      return false;
    }
    static const char *const syscall_registers[] = {
        "\"ebx\"", "\"ecx\"", "\"edx\"", "\"esi\"", "\"edi\""};
    int args[5] = {-1, -1, -1, -1, -1};
    size_t argument_count = 0;
    size_t input_end = p->pos;
    while (inline_asm_string_sequence_is(p, "r", &input_end)) {
      if (argument_count == sizeof(args) / sizeof(args[0]) ||
          !inline_asm_string_sequence_is(p, "r", &p->pos) || !expect(p, "(")) {
        fail(p, "unsupported i386 VDSO syscall argument list");
        return false;
      }
      ny_ctok_t arg_name;
      if (!parse_name(p, &arg_name) || !expect(p, ")")) {
        fail(p, "invalid i386 VDSO syscall argument local");
        return false;
      }
      c_local_t *arg_local = find_local(p, arg_name);
      if (!arg_local || arg_local->array_rank || arg_local->is_const ||
          !arg_local->fixed_register.len ||
          arg_local->fixed_register.len !=
              strlen(syscall_registers[argument_count]) ||
          memcmp(arg_local->fixed_register.start,
                 syscall_registers[argument_count],
                 arg_local->fixed_register.len) != 0 ||
          arg_local->fixed_register_marker >= p->out->len) {
        fail(p, "i386 VDSO syscall argument does not match its ABI register");
        return false;
      }
      p->out->data[arg_local->fixed_register_marker] =
          (nyr_inst_t){.op = NYIR_NOP, .dst = -1, .a = -1, .b = -1};
      arg_local->fixed_register = (ny_ctok_t){0};
      args[argument_count] = lower_local_load(p, arg_local);
      if (args[argument_count++] < 0)
        return false;
      if (!accept(p, ","))
        break;
      input_end = p->pos;
    }
    if (!expect(p, ":") ||
        !inline_asm_string_sequence_is(p, "memory", &p->pos) ||
        !expect(p, ")") || !expect(p, ";")) {
      fail(p, "invalid i386 VDSO syscall clobber or ending");
      return false;
    }
    int number = lower_local_load(p, result_local);
    if (number < 0)
      return false;
    for (size_t i = 0; i < argument_count; ++i) {
      if (args[i] < 0)
        return false;
    }
    int result = emit_value(p, (nyr_inst_t){.op = NYIR_INLINE_ASM,
                                            .dst = -1,
                                            .a = number,
                                            .b = args[0],
                                            .c = args[1],
                                            .d = args[2],
                                            .e = args[3],
                                            .f = args[4],
                                            .imm = -105,
                                            .effects = NYIR_EFFECT_CALL,
                                            .symbol = "syscall_i386"});
    free(assembly);
    return result >= 0 && lower_local_store(p, result_local, result);
  }
  if (assembly_len == sizeof("syscall") - 1 &&
      memcmp(assembly, "syscall", assembly_len) == 0 && accept(p, ":")) {
    size_t output_constraint_end = p->pos;
    bool read_write_number =
        inline_asm_string_sequence_is(p, "+a", &output_constraint_end);
    bool separate_number =
        !read_write_number &&
        inline_asm_string_sequence_is(p, "=a", &output_constraint_end);
    if (!read_write_number && !separate_number) {
      fail(p, "unsupported x86-64 syscall output constraint");
      return false;
    }
    p->pos = output_constraint_end;
    ny_ctok_t result_name;
    if (!expect(p, "(") || !parse_name(p, &result_name) || !expect(p, ")") ||
        !expect(p, ":")) {
      fail(p, "invalid x86-64 syscall output syntax");
      return false;
    }
    c_local_t *result_local = find_local(p, result_name);
    if (!result_local || result_local->array_rank || result_local->is_const ||
        !c_type_is_integer(result_local->type)) {
      fail(p, "invalid x86-64 syscall result local");
      return false;
    }
    c_value_t number = {-1, C_BAD, NULL};
    const char *input_constraints[3];
    size_t argument_count;
    if (read_write_number) {
      input_constraints[0] = "r";
      input_constraints[1] = "r";
      argument_count = 2;
    } else {
      if (!inline_asm_string_sequence_is(p, "0", &p->pos) || !expect(p, "(")) {
        fail(p, "unsupported x86-64 syscall number constraint");
        return false;
      }
      number = parse_expr(p, 1);
      if (number.value < 0 || !c_type_is_integer(number.type) ||
          !expect(p, ")") || !expect(p, ",")) {
        fail(p, "invalid x86-64 syscall number input");
        return false;
      }
      input_constraints[0] = "D";
      input_constraints[1] = "S";
      input_constraints[2] = "d";
      argument_count = 3;
    }
    c_value_t args[3];
    for (size_t i = 0; i < argument_count; ++i) {
      if ((i && !expect(p, ",")) ||
          !inline_asm_string_sequence_is(p, input_constraints[i], &p->pos) ||
          !expect(p, "(")) {
        fail(p, "unsupported x86-64 syscall argument constraint");
        return false;
      }
      args[i] = parse_expr(p, 1);
      if (args[i].value < 0 ||
          (!c_type_is_integer(args[i].type) &&
           !c_type_is_pointer(args[i].type)) ||
          !expect(p, ")")) {
        fail(p, "invalid x86-64 syscall argument expression");
        return false;
      }
    }
    if (!expect(p, ":")) {
      fail(p, "invalid x86-64 syscall clobber section");
      return false;
    }
    static const char *const syscall_clobbers[] = {"rcx", "r11", "memory"};
    for (size_t i = 0;
         i < sizeof(syscall_clobbers) / sizeof(syscall_clobbers[0]); ++i) {
      if ((i && !expect(p, ",")) ||
          !inline_asm_string_sequence_is(p, syscall_clobbers[i], &p->pos)) {
        fail(p, "unsupported x86-64 syscall clobber");
        return false;
      }
    }
    if (!expect(p, ")") || !expect(p, ";")) {
      fail(p, "invalid x86-64 syscall asm statement ending");
      return false;
    }
    static const char *const syscall_registers[] = {"\"rdi\"", "\"rsi\"",
                                                    "\"rdx\""};
    for (size_t i = 0; i < argument_count; ++i) {
      const c_lvalue_t *lvalue = find_lvalue(p, args[i].value);
      c_local_t *local = lvalue && lvalue->is_local
                             ? find_local_slot(p, (size_t)lvalue->local_slot)
                             : NULL;
      if (!local || !local->fixed_register.len)
        continue;
      size_t expected_len = strlen(syscall_registers[i]);
      if (local->fixed_register.len != expected_len ||
          memcmp(local->fixed_register.start, syscall_registers[i],
                 expected_len) != 0 ||
          local->fixed_register_marker >= p->out->len) {
        fail(p, "fixed-register syscall argument does not match its ABI "
                "register");
        return false;
      }
      p->out->data[local->fixed_register_marker] =
          (nyr_inst_t){.op = NYIR_NOP, .dst = -1, .a = -1, .b = -1};
      local->fixed_register = (ny_ctok_t){0};
    }
    int number_value =
        read_write_number ? lower_local_load(p, result_local) : number.value;
    int result =
        number_value < 0
            ? -1
            : emit_value(
                  p, (nyr_inst_t){.op = NYIR_INLINE_ASM,
                                  .dst = -1,
                                  .a = number_value,
                                  .b = args[0].value,
                                  .c = args[1].value,
                                  .d = argument_count == 3 ? args[2].value : -1,
                                  .imm = -20,
                                  .effects = NYIR_EFFECT_CALL,
                                  .symbol = "syscall"});
    free(assembly);
    return result >= 0 && lower_local_store(p, result_local, result);
  }
  static const char lsl_cpunode_asm[] = "lsl %[seg],%k[p]";
  static const char lsl_cpunode_feature[] = ".4byte (16*32+22)";
  bool lsl_cpunode_alternative =
      assembly_len &&
      memmem(assembly, assembly_len, lsl_cpunode_asm,
             sizeof(lsl_cpunode_asm) - 1) &&
      memmem(assembly, assembly_len, "rdpid %[p]", sizeof("rdpid %[p]") - 1) &&
      memmem(assembly, assembly_len, lsl_cpunode_feature,
             sizeof(lsl_cpunode_feature) - 1);
  if (lsl_cpunode_alternative && accept(p, ":")) {
    ny_ctok_t output_operand, output_name, input_operand;
    if (!parse_named_inline_asm_operand(p, &output_operand) ||
        !inline_asm_name_matches(output_operand, (const unsigned char *)"p",
                                 1) ||
        !inline_asm_string_sequence_is(p, "=r", &p->pos) || !expect(p, "(") ||
        !parse_name(p, &output_name) || !expect(p, ")") || !expect(p, ":") ||
        !inline_asm_string_sequence_is(p, "i", &p->pos) || !expect(p, "("))
      return defer_unsupported_asm(p, open, assembly);
    c_value_t alternative_marker = parse_expr(p, 1);
    if (alternative_marker.value < 0 || alternative_marker.value != 0 ||
        !expect(p, ")") || !expect(p, ",") ||
        !parse_named_inline_asm_operand(p, &input_operand) ||
        !inline_asm_name_matches(input_operand, (const unsigned char *)"seg",
                                 3) ||
        !inline_asm_string_sequence_is(p, "r", &p->pos) || !expect(p, "("))
      return defer_unsupported_asm(p, open, assembly);
    c_value_t selector = parse_expr(p, 1);
    if (selector.value < 0 || !c_type_is_integer(selector.type) ||
        !expect(p, ")") || !expect(p, ")") || !expect(p, ";"))
      return defer_unsupported_asm(p, open, assembly);
    c_local_t *output = find_local(p, output_name);
    if (!output || output->array_rank || output->is_const ||
        !c_type_is_integer(output->type))
      return defer_unsupported_asm(p, open, assembly);
    int value = emit_value(p, (nyr_inst_t){.op = NYIR_INLINE_ASM,
                                           .dst = -1,
                                           .a = selector.value,
                                           .b = -1,
                                           .c = -1,
                                           .d = -1,
                                           .e = -1,
                                           .f = -1,
                                           .imm = -19,
                                           .effects = NYIR_EFFECT_VOLATILE,
                                           .symbol = "lsl"});
    free(assembly);
    return value >= 0 && lower_local_store(p, output, value);
  }
  static const char pvclock_scale_asm[] =
      "mulq %[mul_frac] ; shrd $32, %[hi], %[lo]";
  if (assembly_len == sizeof(pvclock_scale_asm) - 1 &&
      memcmp(assembly, pvclock_scale_asm, assembly_len) == 0 &&
      accept(p, ":")) {
    ny_ctok_t lo_operand, hi_operand, lo_name, hi_name;
    if (!parse_named_inline_asm_operand(p, &lo_operand) ||
        !inline_asm_name_matches(lo_operand, (const unsigned char *)"lo", 2) ||
        !inline_asm_string_sequence_is(p, "=a", &p->pos) || !expect(p, "(") ||
        !parse_name(p, &lo_name) || !expect(p, ")") || !expect(p, ",") ||
        !parse_named_inline_asm_operand(p, &hi_operand) ||
        !inline_asm_name_matches(hi_operand, (const unsigned char *)"hi", 2) ||
        !inline_asm_string_sequence_is(p, "=d", &p->pos) || !expect(p, "("))
      return defer_unsupported_asm(p, open, assembly);
    if (!parse_name(p, &hi_name) || !expect(p, ")") || !expect(p, ":") ||
        !inline_asm_string_sequence_is(p, "0", &p->pos) || !expect(p, "("))
      return defer_unsupported_asm(p, open, assembly);
    c_value_t delta = parse_expr(p, 1);
    if (delta.value < 0 || !c_type_is_integer(delta.type) || !expect(p, ")") ||
        !expect(p, ","))
      return defer_unsupported_asm(p, open, assembly);
    ny_ctok_t mul_frac_name;
    if (!parse_named_inline_asm_operand(p, &mul_frac_name) ||
        !inline_asm_name_matches(mul_frac_name,
                                 (const unsigned char *)"mul_frac", 8) ||
        !inline_asm_string_sequence_is(p, "rm", &p->pos) || !expect(p, "("))
      return defer_unsupported_asm(p, open, assembly);
    c_value_t multiplier = parse_expr(p, 1);
    if (multiplier.value < 0 || !c_type_is_integer(multiplier.type) ||
        !expect(p, ")") || !expect(p, ")") || !expect(p, ";"))
      return defer_unsupported_asm(p, open, assembly);
    c_local_t *lo = find_local(p, lo_name);
    c_local_t *hi = find_local(p, hi_name);
    if (!lo || !hi || lo == hi || lo->array_rank || hi->array_rank ||
        lo->is_const || hi->is_const || !c_type_is_integer(lo->type) ||
        !c_type_is_integer(hi->type))
      return defer_unsupported_asm(p, open, assembly);
    const uint64_t low_mask = UINT64_C(0xffffffff);
    int low_product =
        emit_i64_binary(p, NYIR_MUL_I64, delta.value, multiplier.value);
    int high_product = emit_u64_multiply_high(p, delta.value, multiplier.value);
    int low_shifted = lower_bitfield_binary(
        p, NYIR_AND_I64,
        lower_bitfield_binary(p, NYIR_SAR_I64, low_product, 32), low_mask);
    int high_low =
        lower_bitfield_binary(p, NYIR_AND_I64, high_product, low_mask);
    int high_shifted = lower_bitfield_binary(p, NYIR_SHL_I64, high_low, 32);
    int result = emit_i64_binary(p, NYIR_OR_I64, low_shifted, high_shifted);
    free(assembly);
    return low_product >= 0 && high_product >= 0 && low_shifted >= 0 &&
           high_low >= 0 && high_shifted >= 0 && result >= 0 &&
           lower_local_store(p, lo, result) &&
           lower_local_store(p, hi, high_product);
  }
  static const char x86_mulq_asm[] = "mulq %[mul]";
  static const char x86_mulq_positional_asm[] = " mulq %3";
  if (!asm_goto && assembly_len == sizeof(x86_mulq_positional_asm) - 1 &&
      memcmp(assembly, x86_mulq_positional_asm, assembly_len) == 0 &&
      accept(p, ":")) {
    ny_ctok_t low_name, high_name, input_name;
    if (!inline_asm_string_sequence_is(p, "=a", &p->pos) || !expect(p, "(") ||
        !parse_name(p, &low_name) || !expect(p, ")") || !expect(p, ",") ||
        !inline_asm_string_sequence_is(p, "=d", &p->pos) || !expect(p, "(") ||
        !parse_name(p, &high_name) || !expect(p, ")") || !expect(p, ":") ||
        !inline_asm_string_sequence_is(p, "a", &p->pos) || !expect(p, "(") ||
        !parse_name(p, &input_name) || !expect(p, ")") || !expect(p, ",") ||
        !inline_asm_string_sequence_is(p, "rm", &p->pos) || !expect(p, "("))
      return defer_unsupported_asm(p, open, assembly);
    c_value_t multiplier = parse_expr(p, 1);
    if (multiplier.value < 0 || !c_type_is_integer(multiplier.type) ||
        !expect(p, ")") || !expect(p, ")") || !expect(p, ";"))
      return defer_unsupported_asm(p, open, assembly);
    c_local_t *low = find_local(p, low_name);
    c_local_t *high = find_local(p, high_name);
    c_local_t *input = find_local(p, input_name);
    if (!low || !high || !input || low == high ||
        !tok_same(low_name, input_name) || input != low || low->array_rank ||
        high->array_rank || low->is_const || high->is_const ||
        low->type != C_U64 || high->type != C_U64)
      return defer_unsupported_asm(p, open, assembly);
    int left = lower_local_load(p, low);
    int low_product =
        left < 0 ? -1
                 : emit_i64_binary(p, NYIR_MUL_I64, left, multiplier.value);
    int high_product =
        left < 0 ? -1 : emit_u64_multiply_high(p, left, multiplier.value);
    free(assembly);
    return low_product >= 0 && high_product >= 0 &&
           lower_local_store(p, low, low_product) &&
           lower_local_store(p, high, high_product);
  }
  bool i386_pvclock_multiply =
      !asm_goto && assembly_len &&
      memmem(assembly, assembly_len, "mul  %5", sizeof("mul  %5") - 1) &&
      memmem(assembly, assembly_len, "mov  %4,%%eax",
             sizeof("mov  %4,%%eax") - 1) &&
      memmem(assembly, assembly_len, "adc  %5,%%edx",
             sizeof("adc  %5,%%edx") - 1);
  if (i386_pvclock_multiply && accept(p, ":")) {
    ny_ctok_t product_name, tmp1_name, tmp2_name;
    if (!inline_asm_string_sequence_is(p, "=A", &p->pos) || !expect(p, "(") ||
        !parse_name(p, &product_name) || !expect(p, ")") || !expect(p, ",") ||
        !inline_asm_string_sequence_is(p, "=r", &p->pos) || !expect(p, "(") ||
        !parse_name(p, &tmp1_name) || !expect(p, ")") || !expect(p, ",") ||
        !inline_asm_string_sequence_is(p, "=r", &p->pos) || !expect(p, "(") ||
        !parse_name(p, &tmp2_name) || !expect(p, ")") || !expect(p, ":") ||
        !inline_asm_string_sequence_is(p, "a", &p->pos) || !expect(p, "("))
      return defer_unsupported_asm(p, open, assembly);
    c_value_t low = parse_expr(p, 1);
    if (low.value < 0 || !c_type_is_integer(low.type) || !expect(p, ")") ||
        !expect(p, ",") || !inline_asm_string_sequence_is(p, "1", &p->pos) ||
        !expect(p, "("))
      return defer_unsupported_asm(p, open, assembly);
    c_value_t high = parse_expr(p, 1);
    if (high.value < 0 || !c_type_is_integer(high.type) || !expect(p, ")") ||
        !expect(p, ",") || !inline_asm_string_sequence_is(p, "2", &p->pos) ||
        !expect(p, "("))
      return defer_unsupported_asm(p, open, assembly);
    c_value_t fraction = parse_expr(p, 1);
    if (fraction.value < 0 || !c_type_is_integer(fraction.type) ||
        !expect(p, ")") || !expect(p, ")") || !expect(p, ";"))
      return defer_unsupported_asm(p, open, assembly);
    c_local_t *product = find_local(p, product_name);
    c_local_t *tmp1 = find_local(p, tmp1_name);
    c_local_t *tmp2 = find_local(p, tmp2_name);
    if (!product || !tmp1 || !tmp2 || product == tmp1 || product == tmp2 ||
        tmp1 == tmp2 || product->array_rank || tmp1->array_rank ||
        tmp2->array_rank ||
        (product->type != C_U64 && product->type != C_I64) ||
        low.type != C_U32 || high.type != C_U32 || fraction.type != C_U32)
      return defer_unsupported_asm(p, open, assembly);
    const uint64_t mask = UINT64_C(0xffffffff);
    int low_product = lower_bitfield_binary(p, NYIR_AND_I64, low.value, mask);
    int high_product = lower_bitfield_binary(p, NYIR_AND_I64, high.value, mask);
    int fraction_value =
        lower_bitfield_binary(p, NYIR_AND_I64, fraction.value, mask);
    int low_partial =
        low_product < 0 || fraction_value < 0
            ? -1
            : emit_i64_binary(p, NYIR_MUL_I64, low_product, fraction_value);
    int high_partial =
        high_product < 0 || fraction_value < 0
            ? -1
            : emit_i64_binary(p, NYIR_MUL_I64, high_product, fraction_value);
    int low_shifted = low_partial < 0 ? -1
                                      : lower_bitfield_binary(p, NYIR_SHL_I64,
                                                              low_partial, 32);
    int low_carry_signed =
        low_shifted < 0
            ? -1
            : lower_bitfield_binary(p, NYIR_SAR_I64, low_shifted, 32);
    int low_carry =
        low_carry_signed < 0
            ? -1
            : lower_bitfield_binary(p, NYIR_AND_I64, low_carry_signed, mask);
    int high_low = high_partial < 0 ? -1
                                    : lower_bitfield_binary(p, NYIR_AND_I64,
                                                            high_partial, mask);
    int high_shifted =
        high_low < 0 ? -1
                     : lower_bitfield_binary(p, NYIR_SHL_I64, high_low, 32);
    int result =
        low_carry < 0 || high_shifted < 0
            ? -1
            : emit_i64_binary(p, NYIR_ADD_I64, low_carry, high_shifted);
    free(assembly);
    return result >= 0 && lower_local_store(p, product, result);
  }

  static const char x86_delay_loop_asm[] = "\ttest %0,%0\t\n"
                                           "\tjz 3f\t\t\n"
                                           "\tjmp 1f\t\t\n"
                                           ".align 16\t\t\n"
                                           "1:\tjmp 2f\t\t\n"
                                           ".align 16\t\t\n"
                                           "2:\tdec %0\t\t\n"
                                           "\tjnz 2b\t\t\n"
                                           "3:\tdec %0\t\t\n";
  if (!asm_goto && assembly_len == sizeof(x86_delay_loop_asm) - 1 &&
      memcmp(assembly, x86_delay_loop_asm, assembly_len) == 0 &&
      expect(p, ":")) {
    ny_ctok_t output_name;
    if (!inline_asm_string_sequence_is(p, "+a", &p->pos) || !expect(p, "(") ||
        !parse_name(p, &output_name) || !expect(p, ")") || !expect(p, ":") ||
        !expect(p, ")") || !expect(p, ";")) {
      free(assembly);
      return false;
    }
    c_local_t *output = find_local(p, output_name);
    if (!output || output->type != C_U64 || output->array_rank ||
        output->is_const) {
      free(assembly);
      return false;
    }
    int input = lower_local_load(p, output);
    free(assembly);
    if (input < 0)
      return false;
    int result = emit_value(p, (nyr_inst_t){.op = NYIR_INLINE_ASM,
                                            .dst = -1,
                                            .a = input,
                                            .imm = -167,
                                            .symbol = "x86_delay_loop",
                                            .effects = NYIR_EFFECT_VOLATILE});
    return result >= 0 && lower_local_store(p, output, result);
  }

  static const char x86_mull_edx_asm[] = "mull %%edx";
  if (!asm_goto && assembly_len == sizeof(x86_mull_edx_asm) - 1 &&
      memcmp(assembly, x86_mull_edx_asm, assembly_len) == 0 && accept(p, ":")) {
    ny_ctok_t high_name, low_name, tied_high_name;
    if (!inline_asm_string_sequence_is(p, "=d", &p->pos) || !expect(p, "(") ||
        !parse_name(p, &high_name) || !expect(p, ")") || !expect(p, ",") ||
        !inline_asm_string_sequence_is(p, "=&a", &p->pos) || !expect(p, "(") ||
        !parse_name(p, &low_name) || !expect(p, ")") || !expect(p, ":") ||
        !inline_asm_string_sequence_is(p, "1", &p->pos) || !expect(p, "(") ||
        !parse_name(p, &tied_high_name) || !expect(p, ")") || !expect(p, ",") ||
        !inline_asm_string_sequence_is(p, "0", &p->pos) || !expect(p, "("))
      return defer_unsupported_asm(p, open, assembly);
    c_value_t factor = parse_expr(p, 1);
    if (factor.value < 0 || !c_type_is_integer(factor.type) ||
        !expect(p, ")") || !expect(p, ")") || !expect(p, ";"))
      return defer_unsupported_asm(p, open, assembly);
    c_local_t *high = find_local(p, high_name);
    c_local_t *low = find_local(p, low_name);
    if (!high || !low || high == low || !tok_same(high_name, tied_high_name) ||
        high->array_rank || low->array_rank || high->type != C_U64 ||
        low->type != C_I32)
      return defer_unsupported_asm(p, open, assembly);
    int high_input = lower_local_load(p, high);
    int high_operand = high_input < 0
                           ? -1
                           : lower_bitfield_binary(p, NYIR_AND_I64, high_input,
                                                   UINT64_C(0xffffffff));
    int factor_operand = lower_bitfield_binary(p, NYIR_AND_I64, factor.value,
                                               UINT64_C(0xffffffff));
    int product =
        high_operand < 0 || factor_operand < 0
            ? -1
            : emit_i64_binary(p, NYIR_MUL_I64, high_operand, factor_operand);
    int low_result = product < 0
                         ? -1
                         : lower_bitfield_binary(p, NYIR_AND_I64, product,
                                                 UINT64_C(0xffffffff));
    int high_shifted =
        product < 0 ? -1 : lower_bitfield_binary(p, NYIR_SAR_I64, product, 32);
    int high_result = high_shifted < 0
                          ? -1
                          : lower_bitfield_binary(p, NYIR_AND_I64, high_shifted,
                                                  UINT64_C(0xffffffff));
    free(assembly);
    return low_result >= 0 && high_result >= 0 &&
           lower_local_store(p, high, high_result) &&
           lower_local_store(p, low, low_result);
  }

  static const char x86_mull_asm[] = "mull %[b]";
  if (!asm_goto && assembly_len == sizeof(x86_mull_asm) - 1 &&
      memcmp(assembly, x86_mull_asm, assembly_len) == 0 && accept(p, ":")) {
    ny_ctok_t low_name, high_name, low_operand, high_operand;
    ny_ctok_t left_operand, left_name, right_operand;
    if (!parse_named_inline_asm_operand(p, &low_operand) ||
        !inline_asm_string_sequence_is(p, "=a", &p->pos) || !expect(p, "(") ||
        !parse_name(p, &low_name) || !expect(p, ")") || !expect(p, ",") ||
        !parse_named_inline_asm_operand(p, &high_operand) ||
        !inline_asm_string_sequence_is(p, "=d", &p->pos) || !expect(p, "(") ||
        !parse_name(p, &high_name) || !expect(p, ")") || !expect(p, ":") ||
        !parse_named_inline_asm_operand(p, &left_operand) ||
        !inline_asm_name_matches(left_operand, (const unsigned char *)"a", 1) ||
        !inline_asm_string_sequence_is(p, "a", &p->pos) || !expect(p, "(") ||
        !parse_name(p, &left_name) || !expect(p, ")") || !expect(p, ",") ||
        !parse_named_inline_asm_operand(p, &right_operand) ||
        !inline_asm_name_matches(right_operand, (const unsigned char *)"b",
                                 1) ||
        !inline_asm_string_sequence_is(p, "rm", &p->pos) || !expect(p, "("))
      return defer_unsupported_asm(p, open, assembly);
    c_value_t right = parse_expr(p, 1);
    if (right.value < 0 || !c_type_is_integer(right.type) || !expect(p, ")") ||
        !expect(p, ")") || !expect(p, ";"))
      return defer_unsupported_asm(p, open, assembly);
    c_local_t *low = find_local(p, low_name);
    c_local_t *high = find_local(p, high_name);
    c_local_t *left = find_local(p, left_name);
    if (!low || !high || !left || low == high || low == left || high == left ||
        low->array_rank || high->array_rank || left->array_rank ||
        low->type != C_U32 || high->type != C_U32 || left->type != C_U32 ||
        right.type != C_U32)
      return defer_unsupported_asm(p, open, assembly);
    int left_value = lower_local_load(p, left);
    int mask_left = left_value < 0
                        ? -1
                        : lower_bitfield_binary(p, NYIR_AND_I64, left_value,
                                                UINT64_C(0xffffffff));
    int mask_right = lower_bitfield_binary(p, NYIR_AND_I64, right.value,
                                           UINT64_C(0xffffffff));
    int product = mask_left < 0 || mask_right < 0
                      ? -1
                      : emit_i64_binary(p, NYIR_MUL_I64, mask_left, mask_right);
    int low_product = product < 0
                          ? -1
                          : lower_bitfield_binary(p, NYIR_AND_I64, product,
                                                  UINT64_C(0xffffffff));
    int high_shifted =
        product < 0 ? -1 : lower_bitfield_binary(p, NYIR_SAR_I64, product, 32);
    int high_product =
        high_shifted < 0 ? -1
                         : lower_bitfield_binary(p, NYIR_AND_I64, high_shifted,
                                                 UINT64_C(0xffffffff));
    free(assembly);
    return low_product >= 0 && high_product >= 0 &&
           lower_local_store(p, low, low_product) &&
           lower_local_store(p, high, high_product);
  }
  if (!asm_goto && assembly_len == sizeof(x86_mulq_asm) - 1 &&
      memcmp(assembly, x86_mulq_asm, assembly_len) == 0 && accept(p, ":")) {
    ny_ctok_t low_operand, high_operand, low_name, high_name, multiplier_name;
    if (!parse_named_inline_asm_operand(p, &low_operand) ||
        !inline_asm_string_sequence_is(p, "+a", &p->pos) || !expect(p, "(") ||
        !parse_name(p, &low_name) || !expect(p, ")") || !expect(p, ",") ||
        !parse_named_inline_asm_operand(p, &high_operand) ||
        !inline_asm_string_sequence_is(p, "=d", &p->pos) || !expect(p, "(") ||
        !parse_name(p, &high_name) || !expect(p, ")") || !expect(p, ":") ||
        !parse_named_inline_asm_operand(p, &multiplier_name) ||
        !inline_asm_name_matches(multiplier_name, (const unsigned char *)"mul",
                                 3) ||
        !inline_asm_string_sequence_is(p, "rm", &p->pos) || !expect(p, "("))
      return defer_unsupported_asm(p, open, assembly);
    c_value_t multiplier = parse_expr(p, 1);
    if (multiplier.value < 0 || !c_type_is_integer(multiplier.type) ||
        !expect(p, ")") || !expect(p, ")") || !expect(p, ";"))
      return defer_unsupported_asm(p, open, assembly);
    c_local_t *low = find_local(p, low_name);
    c_local_t *high = find_local(p, high_name);
    if (!low || !high || low == high || low->array_rank || high->array_rank ||
        low->is_const || high->is_const || !c_type_is_integer(low->type) ||
        !c_type_is_integer(high->type) ||
        (low->type != C_U64 && low->type != C_I64) ||
        (high->type != C_U64 && high->type != C_I64))
      return defer_unsupported_asm(p, open, assembly);
    int left = lower_local_load(p, low);
    int low_product =
        left < 0 ? -1
                 : emit_i64_binary(p, NYIR_MUL_I64, left, multiplier.value);
    int high_product =
        left < 0 ? -1 : emit_u64_multiply_high(p, left, multiplier.value);
    free(assembly);
    return low_product >= 0 && high_product >= 0 &&
           lower_local_store(p, low, low_product) &&
           lower_local_store(p, high, high_product);
  }
  static const char x86_addq_adcq_ipv6_asm[] =
      "addq %1,%[sum64]\n\tadcq %2,%[sum64]\n"
      "\tadcq %3,%[sum64]\n\tadcq %4,%[sum64]\n"
      "\tadcq $0,%[sum64]";
  size_t checksum_inputs = 0;
  bool checksum_named = false;
  bool checksum_memory = false;
  if (inline_asm_template_is(assembly, assembly_len, x86_addq_adcq_ipv6_asm)) {
    checksum_inputs = 4;
    checksum_named = true;
  } else if (inline_asm_template_is(assembly, assembly_len,
                                    "addq %1,%0\nadcq $0,%0")) {
    checksum_inputs = 1;
  } else if (inline_asm_template_is(assembly, assembly_len,
                                    "addq %[trail],%[res]\nadcq $0,%[res]")) {
    checksum_inputs = 1;
    checksum_named = true;
  } else if (inline_asm_template_is(assembly, assembly_len,
                                    "addq %1,%0\nadcq %2,%0\nadcq %3,%0\n"
                                    "adcq %4,%0\nadcq %5,%0\nadcq $0,%0")) {
    checksum_inputs = 5;
  }
  static const char *const checksum_memory_templates[] = {
      "addq 0*8(%[src]),%[res]\nadcq $0,%[res]",
      "addq 0*8(%[src]),%[res]\nadcq 1*8(%[src]),%[res]\n"
      "adcq $0,%[res]",
      "addq 0*8(%[src]),%[res]\nadcq 1*8(%[src]),%[res]\n"
      "adcq 2*8(%[src]),%[res]\nadcq 3*8(%[src]),%[res]\n"
      "adcq $0,%[res]"};
  for (size_t i = 0; i < 3; ++i) {
    if (inline_asm_template_is(assembly, assembly_len,
                               checksum_memory_templates[i])) {
      checksum_inputs = (size_t)1 << i;
      checksum_named = true;
      checksum_memory = true;
      break;
    }
  }
  if (!asm_goto && checksum_inputs && accept(p, ":")) {
    ny_ctok_t output_operand, output_name;
    if (!parse_named_inline_asm_operand(p, &output_operand) ||
        (checksum_named && !output_operand.len) ||
        !inline_asm_string_sequence_is(p, "+r", &p->pos) || !expect(p, "(") ||
        !parse_name(p, &output_name) || !expect(p, ")") || !expect(p, ":"))
      return defer_unsupported_asm(p, open, assembly);
    int inputs[5] = {-1, -1, -1, -1, -1};
    if (checksum_memory) {
      ny_ctok_t input_operand;
      if (!parse_named_inline_asm_operand(p, &input_operand) ||
          !inline_asm_name_matches(input_operand, (const unsigned char *)"src",
                                   3) ||
          !inline_asm_string_sequence_is(p, "r", &p->pos) || !expect(p, "("))
        return defer_unsupported_asm(p, open, assembly);
      c_value_t address = parse_expr(p, 1);
      if (address.value < 0 || !c_type_is_pointer(address.type) ||
          !expect(p, ")") || !expect(p, ",") ||
          !inline_asm_string_sequence_is(p, "m", &p->pos) || !expect(p, "("))
        return defer_unsupported_asm(p, open, assembly);
      /*
       * The memory operand describes the bytes already read through src.
       */
      size_t end = skip_balanced(p, p->pos - 1, "(", ")");
      if (end <= p->pos || end > p->token_count)
        return defer_unsupported_asm(p, open, assembly);
      p->pos = end;
      for (size_t i = 0; i < checksum_inputs; ++i) {
        int displacement = emit_i64_constant(p, (int64_t)i * 8);
        int element =
            displacement < 0
                ? -1
                : emit_i64_binary(p, NYIR_ADD_I64, address.value, displacement);
        inputs[i] =
            element < 0 ? -1 : lower_load_field(p, element, false, false);
        if (inputs[i] < 0)
          return defer_unsupported_asm(p, open, assembly);
      }
    }
    for (size_t i = 0; !checksum_memory && i < checksum_inputs; ++i) {
      ny_ctok_t input_operand;
      if (!parse_named_inline_asm_operand(p, &input_operand) ||
          !inline_asm_string_sequence_is(p, checksum_inputs == 1 ? "r" : "m",
                                         &p->pos) ||
          !expect(p, "("))
        return defer_unsupported_asm(p, open, assembly);
      c_value_t input = parse_expr(p, 1);
      if (input.value < 0 || !c_type_is_integer(input.type) || !expect(p, ")"))
        return defer_unsupported_asm(p, open, assembly);
      inputs[i] = input.value;
      if (i + 1 != checksum_inputs && !expect(p, ","))
        return defer_unsupported_asm(p, open, assembly);
    }
    if (!expect(p, ")") || !expect(p, ";"))
      return defer_unsupported_asm(p, open, assembly);
    c_local_t *output = find_local(p, output_name);
    if (!output || output->array_rank || output->is_const ||
        (output->type != C_I64 && output->type != C_U64))
      return defer_unsupported_asm(p, open, assembly);
    int accumulator = lower_local_load(p, output);
    int carry = emit_i64_constant(p, 0);
    for (size_t i = 0; accumulator >= 0 && carry >= 0 && i < checksum_inputs;
         ++i) {
      int partial = emit_i64_binary(p, NYIR_ADD_I64, accumulator, inputs[i]);
      int next_carry =
          partial < 0 ? -1
                      : emit_u64_compare(p, partial, accumulator, NYIR_CMP_LT);
      if (i != 0 && partial >= 0 && carry >= 0 && next_carry >= 0) {
        int adjusted = emit_i64_binary(p, NYIR_ADD_I64, partial, carry);
        int adjustment_carry =
            adjusted < 0 ? -1
                         : emit_u64_compare(p, adjusted, partial, NYIR_CMP_LT);
        next_carry =
            adjusted < 0 || adjustment_carry < 0
                ? -1
                : emit_i64_binary(p, NYIR_OR_I64, next_carry, adjustment_carry);
        partial = adjusted;
      }
      accumulator = partial;
      carry = next_carry;
    }
    int result = accumulator < 0 || carry < 0
                     ? -1
                     : emit_i64_binary(p, NYIR_ADD_I64, accumulator, carry);
    free(assembly);
    return result >= 0 && lower_local_store(p, output, result);
  }
  static const char x86_addl_adc_chain_asm[] =
      "addl %1, %0\n  adcl %2, %0\n  adcl %3, %0\n  adcl $0, %0";
  if (!asm_goto &&
      inline_asm_template_equals(assembly, assembly_len,
                                 x86_addl_adc_chain_asm) &&
      accept(p, ":")) {
    size_t output_constraint_end = p->pos;
    ny_ctok_t output_name;
    if (!inline_asm_string_sequence_is(p, "=r", &output_constraint_end))
      return defer_unsupported_asm(p, open, assembly);
    p->pos = output_constraint_end;
    if (!expect(p, "(") || !parse_name(p, &output_name) || !expect(p, ")") ||
        !expect(p, ":"))
      return defer_unsupported_asm(p, open, assembly);
    int addends[3] = {-1, -1, -1};
    for (size_t i = 0; i < 3; ++i) {
      if (!inline_asm_string_sequence_is(p, "g", &p->pos) || !expect(p, "("))
        return defer_unsupported_asm(p, open, assembly);
      c_value_t addend = parse_expr(p, 1);
      if (addend.value < 0 || !c_type_is_integer(addend.type) ||
          !expect(p, ")"))
        return defer_unsupported_asm(p, open, assembly);
      addends[i] = addend.value;
      if (!expect(p, ","))
        return defer_unsupported_asm(p, open, assembly);
    }
    if (!inline_asm_string_sequence_is(p, "0", &p->pos) || !expect(p, "("))
      return defer_unsupported_asm(p, open, assembly);
    c_value_t initial = parse_expr(p, 1);
    if (initial.value < 0 || !c_type_is_integer(initial.type) ||
        !expect(p, ")") || !expect(p, ")") || !expect(p, ";"))
      return defer_unsupported_asm(p, open, assembly);
    c_local_t *output = find_local(p, output_name);
    if (!output || output->array_rank || output->is_const ||
        (output->type != C_I32 && output->type != C_U32))
      return defer_unsupported_asm(p, open, assembly);
    int accumulator = normalize_u32(p, initial.value);
    int carry = emit_i64_constant(p, 0);
    for (size_t i = 0; accumulator >= 0 && carry >= 0 && i < 3; ++i) {
      int addend = normalize_u32(p, addends[i]);
      int partial = addend < 0
                        ? -1
                        : emit_i64_binary(p, NYIR_ADD_I64, accumulator, addend);
      int narrowed = partial < 0 ? -1 : normalize_u32(p, partial);
      int next_carry = narrowed < 0 ? -1
                                    : emit_i64_compare(p, narrowed, accumulator,
                                                       NYIR_CMP_LT);
      if (i != 0 && narrowed >= 0 && carry >= 0 && next_carry >= 0) {
        int adjusted_sum = emit_i64_binary(p, NYIR_ADD_I64, narrowed, carry);
        int adjusted = adjusted_sum < 0 ? -1 : normalize_u32(p, adjusted_sum);
        int adjustment_carry =
            adjusted < 0 ? -1
                         : emit_i64_compare(p, adjusted, narrowed, NYIR_CMP_LT);
        next_carry =
            adjusted < 0 || adjustment_carry < 0
                ? -1
                : emit_i64_binary(p, NYIR_OR_I64, next_carry, adjustment_carry);
        narrowed = adjusted;
      }
      accumulator = narrowed;
      carry = next_carry;
    }
    int final_sum = accumulator < 0 || carry < 0
                        ? -1
                        : emit_i64_binary(p, NYIR_ADD_I64, accumulator, carry);
    int result = final_sum < 0 ? -1 : normalize_u32(p, final_sum);
    free(assembly);
    return result >= 0 && lower_local_store(p, output, result);
  }
  bool addl_adcl_ffff = inline_asm_template_equals(
      assembly, assembly_len, "addl %1,%0\n  adcl $0xffff,%0");
  bool addl_adcl_zero = inline_asm_template_equals(assembly, assembly_len,
                                                   "addl %2,%0\n\tadcl $0,%0");
  if (!asm_goto && (addl_adcl_ffff || addl_adcl_zero) && accept(p, ":")) {
    size_t output_constraint_end = p->pos;
    ny_ctok_t output_name;
    if (!inline_asm_string_sequence_is(p, "=r", &output_constraint_end))
      return defer_unsupported_asm(p, open, assembly);
    p->pos = output_constraint_end;
    if (!expect(p, "(") || !parse_name(p, &output_name) || !expect(p, ")") ||
        !expect(p, ":"))
      return defer_unsupported_asm(p, open, assembly);
    c_value_t addend = {-1, C_BAD, NULL};
    c_value_t initial = {-1, C_BAD, NULL};
    if (addl_adcl_ffff) {
      if (!inline_asm_string_sequence_is(p, "r", &p->pos) || !expect(p, "("))
        return defer_unsupported_asm(p, open, assembly);
      addend = parse_expr(p, 1);
      if (addend.value < 0 || !c_type_is_integer(addend.type) ||
          !expect(p, ")") || !expect(p, ",") ||
          !inline_asm_string_sequence_is(p, "0", &p->pos) || !expect(p, "("))
        return defer_unsupported_asm(p, open, assembly);
      initial = parse_expr(p, 1);
    } else {
      if (!inline_asm_string_sequence_is(p, "0", &p->pos) || !expect(p, "("))
        return defer_unsupported_asm(p, open, assembly);
      initial = parse_expr(p, 1);
      if (initial.value < 0 || !c_type_is_integer(initial.type) ||
          !expect(p, ")") || !expect(p, ",") ||
          !inline_asm_string_sequence_is(p, "rm", &p->pos) || !expect(p, "("))
        return defer_unsupported_asm(p, open, assembly);
      addend = parse_expr(p, 1);
    }
    if (addend.value < 0 || !c_type_is_integer(addend.type) ||
        initial.value < 0 || !c_type_is_integer(initial.type) ||
        !expect(p, ")") || !expect(p, ")") || !expect(p, ";"))
      return defer_unsupported_asm(p, open, assembly);
    c_local_t *output = find_local(p, output_name);
    if (!output || output->array_rank || output->is_const ||
        (output->type != C_I32 && output->type != C_U32))
      return defer_unsupported_asm(p, open, assembly);
    int initial_u32 = normalize_u32(p, initial.value);
    int addend_u32 = normalize_u32(p, addend.value);
    int sum = initial_u32 < 0 || addend_u32 < 0
                  ? -1
                  : emit_i64_binary(p, NYIR_ADD_I64, initial_u32, addend_u32);
    int sum_u32 = sum < 0 ? -1 : normalize_u32(p, sum);
    int carry = sum_u32 < 0
                    ? -1
                    : emit_i64_compare(p, sum_u32, initial_u32, NYIR_CMP_LT);
    int correction = emit_i64_constant(p, addl_adcl_ffff ? UINT16_MAX : 0);
    int correction_with_carry =
        correction < 0 || carry < 0
            ? -1
            : emit_i64_binary(p, NYIR_ADD_I64, correction, carry);
    int result =
        sum_u32 < 0 || correction_with_carry < 0
            ? -1
            : emit_i64_binary(p, NYIR_ADD_I64, sum_u32, correction_with_carry);
    if (result >= 0)
      result = normalize_u32(p, result);
    free(assembly);
    return result >= 0 && lower_local_store(p, output, result);
  }
  static const char x86_addq_adcq_asm[] = "addq %[add], %[lo]; adcq $0, %[hi]";
  if (!asm_goto && assembly_len == sizeof(x86_addq_adcq_asm) - 1 &&
      memcmp(assembly, x86_addq_adcq_asm, assembly_len) == 0 &&
      accept(p, ":")) {
    ny_ctok_t lo_operand, hi_operand, add_operand, lo_name, hi_name;
    if (!parse_named_inline_asm_operand(p, &lo_operand) ||
        !inline_asm_name_matches(lo_operand, (const unsigned char *)"lo", 2) ||
        !inline_asm_string_sequence_is(p, "+r", &p->pos) || !expect(p, "(") ||
        !parse_name(p, &lo_name) || !expect(p, ")") || !expect(p, ",") ||
        !parse_named_inline_asm_operand(p, &hi_operand) ||
        !inline_asm_name_matches(hi_operand, (const unsigned char *)"hi", 2) ||
        !inline_asm_string_sequence_is(p, "+r", &p->pos) || !expect(p, "(") ||
        !parse_name(p, &hi_name) || !expect(p, ")") || !expect(p, ":") ||
        !parse_named_inline_asm_operand(p, &add_operand) ||
        !inline_asm_name_matches(add_operand, (const unsigned char *)"add",
                                 3) ||
        !inline_asm_string_sequence_is(p, "erm", &p->pos) || !expect(p, "("))
      return defer_unsupported_asm(p, open, assembly);
    c_value_t addend = parse_expr(p, 1);
    if (addend.value < 0 || !c_type_is_integer(addend.type) ||
        !expect(p, ")") || !expect(p, ")") || !expect(p, ";"))
      return defer_unsupported_asm(p, open, assembly);
    c_local_t *lo = find_local(p, lo_name);
    c_local_t *hi = find_local(p, hi_name);
    if (!lo || !hi || lo == hi || lo->array_rank || hi->array_rank ||
        lo->is_const || hi->is_const ||
        (lo->type != C_U64 && lo->type != C_I64) ||
        (hi->type != C_U64 && hi->type != C_I64))
      return defer_unsupported_asm(p, open, assembly);
    int lo_value = lower_local_load(p, lo);
    int hi_value = lower_local_load(p, hi);
    int lo_sum = lo_value < 0
                     ? -1
                     : emit_i64_binary(p, NYIR_ADD_I64, lo_value, addend.value);
    int carry =
        lo_sum < 0 ? -1 : emit_u64_compare(p, lo_sum, lo_value, NYIR_CMP_LT);
    int hi_sum = hi_value < 0 || carry < 0
                     ? -1
                     : emit_i64_binary(p, NYIR_ADD_I64, hi_value, carry);
    free(assembly);
    return lo_sum >= 0 && carry >= 0 && hi_sum >= 0 &&
           lower_local_store(p, lo, lo_sum) && lower_local_store(p, hi, hi_sum);
  }
  if (!asm_goto && inline_asm_template_is(assembly, assembly_len, "divl %2")) {
    static const char *const outputs[] = {"=a", "=d"};
    int addresses[2], inputs[3];
    if (!expect(p, ":")) {
      free(assembly);
      return false;
    }
    for (size_t i = 0; i < 2; ++i) {
      if ((i && !expect(p, ",")) || !parse_inline_asm_operand_name(p) ||
          !inline_asm_string_sequence_is(p, outputs[i], &p->pos) ||
          !expect(p, "(")) {
        free(assembly);
        return false;
      }
      c_value_t output = parse_expr(p, 1);
      const c_lvalue_t *lvalue =
          output.value >= 0 ? find_lvalue(p, output.value) : NULL;
      if (!c_type_is_integer(output.type) ||
          c_type_size(p->abi, output.type) != 4 ||
          (addresses[i] = lower_asm_memory_output_address(p, lvalue)) < 0 ||
          !expect(p, ")")) {
        free(assembly);
        return false;
      }
    }
    static const char *const constraints[] = {"rm", "0", "1"};
    if (!expect(p, ":")) {
      free(assembly);
      return false;
    }
    for (size_t i = 0; i < 3; ++i) {
      if ((i && !expect(p, ",")) || !parse_inline_asm_operand_name(p) ||
          !inline_asm_string_sequence_is(p, constraints[i], &p->pos) ||
          !expect(p, "(")) {
        free(assembly);
        return false;
      }
      c_value_t input = parse_expr(p, 1);
      if (input.value < 0 || !c_type_is_integer(input.type) ||
          c_type_size(p->abi, input.type) != 4 || !expect(p, ")")) {
        free(assembly);
        return false;
      }
      inputs[i] = input.value;
    }
    if ((accept(p, ":") && !inline_asm_clobber_list_is(p, &p->pos)) ||
        !expect(p, ")") || !expect(p, ";")) {
      free(assembly);
      return false;
    }
    free(assembly);
    return emit(p,
                (nyr_inst_t){
                    .op = NYIR_INLINE_ASM,
                    .dst = -1,
                    .a = inputs[1],
                    .b = inputs[2],
                    .c = inputs[0],
                    .d = addresses[0],
                    .e = addresses[1],
                    .imm = -190,
                    .symbol = "divl_u32",
                    .effects = NYIR_EFFECT_READ_MEMORY |
                               NYIR_EFFECT_WRITE_MEMORY | NYIR_EFFECT_MAY_TRAP |
                               NYIR_EFFECT_CONTROL | NYIR_EFFECT_VOLATILE},
                NULL);
  }
  static const char x86_divq_asm[] = "divq %[div]";
  if (!asm_goto && assembly_len == sizeof(x86_divq_asm) - 1 &&
      memcmp(assembly, x86_divq_asm, assembly_len) == 0 && accept(p, ":")) {
    ny_ctok_t low_operand, high_operand, div_operand;
    ny_ctok_t low_name, high_name;
    if (!parse_named_inline_asm_operand(p, &low_operand) ||
        !inline_asm_string_sequence_is(p, "+a", &p->pos) || !expect(p, "(") ||
        !parse_name(p, &low_name) || !expect(p, ")") || !expect(p, ",") ||
        !parse_named_inline_asm_operand(p, &high_operand) ||
        !inline_asm_string_sequence_is(p, "+d", &p->pos) || !expect(p, "(") ||
        !parse_name(p, &high_name) || !expect(p, ")") || !expect(p, ":") ||
        !parse_named_inline_asm_operand(p, &div_operand) ||
        !inline_asm_name_matches(div_operand, (const unsigned char *)"div",
                                 3) ||
        !inline_asm_string_sequence_is(p, "rm", &p->pos) || !expect(p, "("))
      return defer_unsupported_asm(p, open, assembly);
    c_value_t divisor = parse_expr(p, 1);
    if (divisor.value < 0 || !c_type_is_integer(divisor.type) ||
        !expect(p, ")") || !expect(p, ")") || !expect(p, ";"))
      return defer_unsupported_asm(p, open, assembly);
    c_local_t *low = find_local(p, low_name);
    c_local_t *high = find_local(p, high_name);
    if (!low || !high || low == high || low->array_rank || high->array_rank ||
        low->is_const || high->is_const ||
        (low->type != C_U64 && low->type != C_I64) ||
        (high->type != C_U64 && high->type != C_I64) ||
        (divisor.type != C_U64 && divisor.type != C_I64))
      return defer_unsupported_asm(p, open, assembly);
    int low_value = lower_local_load(p, low);
    int high_value = lower_local_load(p, high);
    int quotient =
        low_value < 0 || high_value < 0
            ? -1
            : emit_value(p, (nyr_inst_t){.op = NYIR_INLINE_ASM,
                                         .dst = -1,
                                         .a = low_value,
                                         .b = high_value,
                                         .c = divisor.value,
                                         .imm = -106,
                                         .symbol = "divq_u64",
                                         .effects = NYIR_EFFECT_CONTROL |
                                                    NYIR_EFFECT_MAY_TRAP |
                                                    NYIR_EFFECT_VOLATILE});
    int product = quotient < 0 ? -1
                               : emit_i64_binary(p, NYIR_MUL_I64, quotient,
                                                 divisor.value);
    int remainder = low_value < 0 || product < 0
                        ? -1
                        : emit_i64_binary(p, NYIR_SUB_I64, low_value, product);
    free(assembly);
    return quotient >= 0 && remainder >= 0 &&
           lower_local_store(p, low, quotient) &&
           lower_local_store(p, high, remainder);
  }
  static const char rdtsc_ordered_alt_features[] = ".4byte (20*32+ 2)";
  bool rdtsc_plain = assembly_len == sizeof(rdtsc_asm) - 1 &&
                     memcmp(assembly, rdtsc_asm, assembly_len) == 0;
  bool rdtsc_ordered_alternative =
      INLINE_ASM_HAS("# ALT: oldinstr") && INLINE_ASM_HAS("lfence; rdtsc") &&
      INLINE_ASM_HAS("rdtscp") && INLINE_ASM_HAS(rdtsc_ordered_alt_features);
  if ((rdtsc_plain || rdtsc_ordered_alternative) && p->abi == NYC_ABI_I386 &&
      accept(p, ":")) {
    ny_ctok_t value_name;
    if (!inline_asm_string_sequence_is(p, "=A", &p->pos) || !expect(p, "(") ||
        !parse_name(p, &value_name) || !expect(p, ")") ||
        (rdtsc_ordered_alternative &&
         (!expect(p, ":") || !expect(p, ":") ||
          !inline_asm_string_sequence_is(p, "ecx", &p->pos))) ||
        !expect(p, ")") || !expect(p, ";")) {
      free(assembly);
      return false;
    }
    c_local_t *value_local = find_local(p, value_name);
    if (!value_local ||
        (value_local->type != C_U64 && value_local->type != C_I64) ||
        value_local->is_const || value_local->array_rank) {
      fail(p, "unsupported i386 rdtsc output operand");
      free(assembly);
      return false;
    }
    free(assembly);
    int timestamp =
        emit_value(p, (nyr_inst_t){.op = NYIR_INLINE_ASM,
                                   .dst = -1,
                                   .a = -1,
                                   .b = -1,
                                   .c = -1,
                                   .d = -1,
                                   .imm = rdtsc_ordered_alternative ? -18 : -17,
                                   .effects = NYIR_EFFECT_VOLATILE,
                                   .symbol = "rdtsc"});
    return timestamp >= 0 && lower_local_store(p, value_local, timestamp);
  }
  if ((rdtsc_plain || rdtsc_ordered_alternative) && accept(p, ":")) {
    size_t constraint_end = p->pos;
    if (!inline_asm_string_sequence_is(p, "=a", &constraint_end))
      return defer_unsupported_asm(p, open, assembly);
    p->pos = constraint_end;
    ny_ctok_t low_name;
    if (!expect(p, "(") || !parse_name(p, &low_name) || !expect(p, ")") ||
        !expect(p, ","))
      return defer_unsupported_asm(p, open, assembly);
    constraint_end = p->pos;
    if (!inline_asm_string_sequence_is(p, "=d", &constraint_end))
      return defer_unsupported_asm(p, open, assembly);
    p->pos = constraint_end;
    ny_ctok_t high_name;
    if (!expect(p, "(") || !parse_name(p, &high_name) || !expect(p, ")"))
      return defer_unsupported_asm(p, open, assembly);
    if (rdtsc_ordered_alternative) {
      if (!expect(p, ":") || !expect(p, ":"))
        return defer_unsupported_asm(p, open, assembly);
      size_t clobber_end = p->pos;
      if (!inline_asm_string_sequence_is(p, "ecx", &clobber_end))
        return defer_unsupported_asm(p, open, assembly);
      p->pos = clobber_end;
    }
    if (!expect(p, ")") || !expect(p, ";"))
      return defer_unsupported_asm(p, open, assembly);
    c_local_t *low = find_local(p, low_name);
    c_local_t *high = find_local(p, high_name);
    if (!low || !high || low == high || low->array_rank || high->array_rank ||
        low->is_const || high->is_const || !c_type_is_integer(low->type) ||
        !c_type_is_integer(high->type))
      return defer_unsupported_asm(p, open, assembly);
    int timestamp =
        emit_value(p, (nyr_inst_t){.op = NYIR_INLINE_ASM,
                                   .dst = -1,
                                   .a = -1,
                                   .b = -1,
                                   .c = -1,
                                   .d = -1,
                                   .imm = rdtsc_ordered_alternative ? -18 : -17,
                                   .effects = NYIR_EFFECT_VOLATILE,
                                   .symbol = "rdtsc"});
    int mask = timestamp < 0 ? -1 : emit_i64_constant(p, UINT32_MAX);
    int low_value =
        mask < 0 ? -1 : emit_i64_binary(p, NYIR_AND_I64, timestamp, mask);
    int high_shifted =
        timestamp < 0 ? -1
                      : lower_bitfield_binary(p, NYIR_SAR_I64, timestamp, 32);
    int high_value = high_shifted < 0 || mask < 0
                         ? -1
                         : emit_i64_binary(p, NYIR_AND_I64, high_shifted, mask);
    bool ok = low_value >= 0 && high_value >= 0 &&
              lower_local_store(p, low, low_value) &&
              lower_local_store(p, high, high_value);
    free(assembly);
    return ok;
  }

  c_local_t *asm_output = NULL;
  c_local_t *asm_input = NULL;
  char immediate_storage[16][128];
  const char *immediate_values[16];
  size_t immediate_count = 0;
  inline_asm_named_value_t asm_named_inputs[16];
  size_t asm_named_input_count = 0;
  size_t memory_operand_index = SIZE_MAX;
  int asm_memory_input_value = -1;
  int asm_memory_output_value = -1;
  int asm_memory_register_input_value = -1;
  bool asm_memory_register_xadd_unlocked = false;
  bool asm_memory_register_xadd_gs = false;
  int asm_fixed_register_inputs[3] = {-1, -1, -1};
  bool lower_rip_relative_pointer = false;
  bool lower_x86_register_snapshot = false;
  c_lvalue_t x86_snapshot_outputs[3] = {{0}};
  bool lower_tied_register_copy = false;
  bool lower_runtime_const_ptr = false;
  bool lower_runtime_const_shift = false;
  bool lower_runtime_ptr_clamp = false;
  int runtime_ptr_clamp_bound = -1;
  int runtime_ptr_clamp_initial = -1;
  char runtime_shift_section[128] = {0};
  bool lower_named_register_output = false;
  bool lower_named_register_memory_input = false;
  bool lower_condition_register_output = false;
  bool lower_named_memory_output = false;
  bool lower_memory_register_output = false;
  int64_t asm_memory_register_imm = -10;
  bool lower_cmpxchg_outputs = false;
  bool lower_cmpxchg_value_output = false;
  bool lower_cmpxchg64 = false;
  bool lower_cmpxchg8 = false;
  bool lower_cmpxchg16 = false;
  bool lower_classic_cmpxchg_locked = true;
  int asm_cmpxchg_new_value = -1;
  int asm_cmpxchg_expected_value = -1;
  int asm_tzcnt_input = -1;
  int asm_far_jump_memory = -1;
  int asm_far_jump_type = -1;
  bool lower_far_jump = false;
  int asm_segment_load_value = -1;
  bool lower_segment_load_asm = false;
  int asm_indirect_jump_target = -1;
  int asm_indirect_jump_argument = -1;
  bool lower_indirect_jump = false;
  c_local_t *asm_alt_io_output = NULL;
  int asm_alt_io_input = -1;
  int asm_alt_io_memory = -1;
  bool lower_apic_alt_io = false;
  int asm_bsr_input = -1;
  int asm_bsf_input = -1;
  bool lower_tzcnt = false;
  bool lower_bsr = false;
  bool lower_bsf = false;
  int asm_debug_input = -1;
  int asm_debug_reg = -1;
  bool lower_atomic_inc = false;
  bool lower_atomic_dec = false;
  bool lower_atomic_memory_rmw = false;
  bool lower_atomic_memory_rmw64 = false;
  bool lower_atomic_bitwise_condition = false;
  bool lower_atomic_bts_condition = false;
  bool lower_bt_condition = false;
  const char *bt_condition_symbol = NULL;
  bool lower_atomic_arithmetic_condition = false;
  const char *atomic_memory_rmw_mnemonic = NULL;
  bool atomic_memory_rmw_locked = true;
  const char *atomic_arithmetic_mnemonic = NULL;
  bool atomic_arithmetic_locked = true;
  bool lower_fixed_register_input = false;
  bool lower_invpcid = false;
  bool lower_empty_memory_barrier = false;
  c_local_t *asm_condition_output = NULL;
  bool lower_named_memory_condition_output = false;
  int tied_input_value = -1;
  bool lower_classic_cmpxchg =
      (assembly_len == sizeof("lock cmpxchgq %2, %1") - 1 &&
       memcmp(assembly, "lock cmpxchgq %2, %1", assembly_len) == 0) ||
      (assembly_len == sizeof("cmpxchgq %2, %1") - 1 &&
       memcmp(assembly, "cmpxchgq %2, %1", assembly_len) == 0) ||
      (assembly_len == sizeof("lock cmpxchgl %2, %1") - 1 &&
       memcmp(assembly, "lock cmpxchgl %2, %1", assembly_len) == 0) ||
      (assembly_len == sizeof("lock cmpxchgw %2, %1") - 1 &&
       memcmp(assembly, "lock cmpxchgw %2, %1", assembly_len) == 0) ||
      (assembly_len == sizeof("lock cmpxchgb %2, %1") - 1 &&
       memcmp(assembly, "lock cmpxchgb %2, %1", assembly_len) == 0);
  lower_classic_cmpxchg_locked =
      assembly_len >= sizeof("lock ") - 1 &&
      memcmp(assembly, "lock ", sizeof("lock ") - 1) == 0;
  bool apic_alt_io =
      INLINE_ASM_HAS("# ALT: oldinstr") && INLINE_ASM_HAS("movl %0, %1") &&
      INLINE_ASM_HAS("xchgl %0, %1") && INLINE_ASM_HAS(".altinstructions");
  bool kexec_segment_load =
      INLINE_ASM_HAS("movl %0,%%ds") && INLINE_ASM_HAS("movl %0,%%es") &&
      INLINE_ASM_HAS("movl %0,%%ss") && INLINE_ASM_HAS("movl %0,%%fs") &&
      INLINE_ASM_HAS("movl %0,%%gs");
  const char *memory_read_constraint = NULL;
  c_type_t memory_read_type = C_BAD;
  uint32_t memory_read_flags = 0;
  if (assembly_len == sizeof("movb %1,%0") - 1 &&
      memcmp(assembly, "movb %1,%0", assembly_len) == 0) {
    memory_read_constraint = "=q";
    memory_read_type = C_U8;
    memory_read_flags = NYIR_INST_F_MEM_BYTE;
  } else if (assembly_len == sizeof("movw %1,%0") - 1 &&
             memcmp(assembly, "movw %1,%0", assembly_len) == 0) {
    memory_read_constraint = "=r";
    memory_read_type = C_U16;
    memory_read_flags = NYIR_INST_F_MEM_U16;
  } else if (assembly_len == sizeof("movl %1,%0") - 1 &&
             memcmp(assembly, "movl %1,%0", assembly_len) == 0) {
    memory_read_constraint = "=r";
    memory_read_type = C_U32;
    memory_read_flags = NYIR_INST_F_MEM_I32;
  }
  if (memory_read_constraint) {
    size_t constraint_end = p->pos;
    ny_ctok_t output_name;
    if (!expect(p, ":") || !inline_asm_string_sequence_is(
                               p, memory_read_constraint, &constraint_end))
      return defer_unsupported_asm(p, open, assembly);
    p->pos = constraint_end;
    if (!expect(p, "(") || !parse_name(p, &output_name) || !expect(p, ")") ||
        !expect(p, ":") || !inline_asm_string_sequence_is(p, "m", &p->pos) ||
        !expect(p, "("))
      return defer_unsupported_asm(p, open, assembly);
    c_value_t source = parse_expr(p, 1);
    const c_lvalue_t *source_lvalue =
        source.value >= 0 ? find_lvalue(p, source.value) : NULL;
    if (!source_lvalue || source_lvalue->address < 0 ||
        source_lvalue->type != memory_read_type || !expect(p, ")"))
      return defer_unsupported_asm(p, open, assembly);
    if (accept(p, ":")) {
      size_t clobber_end = p->pos;
      if (!inline_asm_clobber_list_is(p, &clobber_end))
        return defer_unsupported_asm(p, open, assembly);
      p->pos = clobber_end;
    }
    if (!expect(p, ")") || !expect(p, ";"))
      return defer_unsupported_asm(p, open, assembly);
    c_local_t *output = find_local(p, output_name);
    if (!output || output->array_rank || output->is_const ||
        output->type != memory_read_type) {
      free(assembly);
      fail(p, "unsupported narrow memory-read inline-assembly output");
      return false;
    }
    int loaded = emit_value(
        p, (nyr_inst_t){.op = NYIR_LOAD_I64,
                        .dst = -1,
                        .a = source_lvalue->address,
                        .effects = NYIR_EFFECT_READ_MEMORY |
                                   NYIR_EFFECT_MAY_TRAP | NYIR_EFFECT_VOLATILE,
                        .flags = memory_read_flags,
                        .scalar_class = NYIR_SCALAR_I64_RAW});
    if (memory_read_type == C_U16 && loaded >= 0)
      loaded = normalize_u16(p, loaded);
    else if (memory_read_type == C_U32 && loaded >= 0)
      loaded = normalize_u32(p, loaded);
    bool ok = loaded >= 0 && lower_local_store(p, output, loaded);
    free(assembly);
    return ok;
  }
  /*
   * `movb/movw/movl (%1),%%al/ax/eax` dereference a pointer held in a general
   * register instead of using an `m` operand, so the matcher above cannot
   * express them. They still load one value from that address; model each as
   * a load through the pointer value so every target keeps the same fault and
   * volatile semantics.
   */
  const char *register_load_template = NULL;
  c_type_t register_load_type = C_BAD;
  uint32_t register_load_flags = 0;
  if (assembly_len == sizeof("movb (%1),%%al") - 1 &&
      memcmp(assembly, "movb (%1),%%al", assembly_len) == 0) {
    register_load_template = "movb (%1),%%al";
    register_load_type = C_U8;
    register_load_flags = NYIR_INST_F_MEM_BYTE;
  } else if (assembly_len == sizeof("movw (%1),%%ax") - 1 &&
             memcmp(assembly, "movw (%1),%%ax", assembly_len) == 0) {
    register_load_template = "movw (%1),%%ax";
    register_load_type = C_U16;
    register_load_flags = NYIR_INST_F_MEM_U16;
  } else if (assembly_len == sizeof("movl (%1),%%eax") - 1 &&
             memcmp(assembly, "movl (%1),%%eax", assembly_len) == 0) {
    register_load_template = "movl (%1),%%eax";
    register_load_type = C_U32;
    register_load_flags = NYIR_INST_F_MEM_I32;
  }
  if (register_load_template) {
    ny_ctok_t output_name;
    if (!expect(p, ":") || !inline_asm_string_sequence_is(p, "=a", &p->pos) ||
        !expect(p, "(") || !parse_name(p, &output_name) || !expect(p, ")") ||
        !expect(p, ":") || !inline_asm_string_sequence_is(p, "r", &p->pos) ||
        !expect(p, "("))
      return defer_unsupported_asm(p, open, assembly);
    c_value_t pointer = parse_expr(p, 1);
    if (!expect(p, ")"))
      return defer_unsupported_asm(p, open, assembly);
    if (accept(p, ":")) {
      size_t clobber_end = p->pos;
      if (!inline_asm_clobber_list_is(p, &clobber_end))
        return defer_unsupported_asm(p, open, assembly);
      p->pos = clobber_end;
    }
    if (!expect(p, ")") || !expect(p, ";"))
      return defer_unsupported_asm(p, open, assembly);
    c_local_t *output = find_local(p, output_name);
    if (pointer.value < 0 || !c_type_is_pointer(pointer.type) || !output ||
        output->array_rank || output->is_const ||
        output->type != register_load_type) {
      free(assembly);
      fail(p, "unsupported register-addressed load inline-assembly operand");
      return false;
    }
    int loaded = emit_value(
        p, (nyr_inst_t){.op = NYIR_LOAD_I64,
                        .dst = -1,
                        .a = pointer.value,
                        .effects = NYIR_EFFECT_READ_MEMORY |
                                   NYIR_EFFECT_MAY_TRAP | NYIR_EFFECT_VOLATILE,
                        .flags = register_load_flags,
                        .scalar_class = NYIR_SCALAR_I64_RAW});
    if (register_load_type == C_U16 && loaded >= 0)
      loaded = normalize_u16(p, loaded);
    else if (register_load_type == C_U32 && loaded >= 0)
      loaded = normalize_u32(p, loaded);
    bool ok = loaded >= 0 && lower_local_store(p, output, loaded);
    free(assembly);
    return ok;
  }
  const char *memory_write_constraint = NULL;
  c_type_t memory_write_type = C_BAD;
  uint32_t memory_write_flags = 0;
  if (assembly_len == sizeof("movb %0,%1") - 1 &&
      memcmp(assembly, "movb %0,%1", assembly_len) == 0) {
    memory_write_constraint = "q";
    memory_write_type = C_U8;
    memory_write_flags = NYIR_INST_F_MEM_BYTE;
  } else if (assembly_len == sizeof("movw %0,%1") - 1 &&
             memcmp(assembly, "movw %0,%1", assembly_len) == 0) {
    memory_write_constraint = "r";
    memory_write_type = C_U16;
    memory_write_flags = NYIR_INST_F_MEM_U16;
  } else if (assembly_len == sizeof("movq %0,%1") - 1 &&
             memcmp(assembly, "movq %0,%1", assembly_len) == 0) {
    memory_write_constraint = "r";
    memory_write_type = C_U64;
  }
  if (memory_write_constraint) {
    if (!expect(p, ":") || !expect(p, ":") ||
        !inline_asm_string_sequence_is(p, memory_write_constraint, &p->pos) ||
        !expect(p, "("))
      return defer_unsupported_asm(p, open, assembly);
    c_value_t value = parse_expr(p, 1);
    if (value.value < 0 || value.type != memory_write_type || !expect(p, ")") ||
        !expect(p, ",") || !inline_asm_string_sequence_is(p, "m", &p->pos) ||
        !expect(p, "("))
      return defer_unsupported_asm(p, open, assembly);
    c_value_t target = parse_expr(p, 1);
    const c_lvalue_t *target_lvalue =
        target.value >= 0 ? find_lvalue(p, target.value) : NULL;
    if (!target_lvalue || target_lvalue->address < 0 ||
        target_lvalue->type != memory_write_type || !expect(p, ")"))
      return defer_unsupported_asm(p, open, assembly);
    if (accept(p, ":")) {
      size_t clobber_end = p->pos;
      if (!inline_asm_clobber_list_is(p, &clobber_end))
        return defer_unsupported_asm(p, open, assembly);
      p->pos = clobber_end;
    }
    if (!expect(p, ")") || !expect(p, ";"))
      return defer_unsupported_asm(p, open, assembly);
    bool ok = emit(p,
                   (nyr_inst_t){.op = NYIR_STORE_I64,
                                .dst = -1,
                                .a = target_lvalue->address,
                                .c = value.value,
                                .effects = NYIR_EFFECT_WRITE_MEMORY |
                                           NYIR_EFFECT_MAY_TRAP |
                                           NYIR_EFFECT_VOLATILE,
                                .flags = memory_write_flags,
                                .scalar_class = NYIR_SCALAR_I64_RAW},
                   NULL);
    free(assembly);
    return ok;
  }
  /*
   * `movb/movw/movl %%al/ax/eax,(%1)` store to an address held in a general
   * register. The value is an `a` input and the pointer an `r` input, so this
   * is a width-specific store through the pointer value.
   */
  const char *register_store_template = NULL;
  c_type_t register_store_type = C_BAD;
  uint32_t register_store_flags = 0;
  if (assembly_len == sizeof("movb %%al,(%1)") - 1 &&
      memcmp(assembly, "movb %%al,(%1)", assembly_len) == 0) {
    register_store_template = "movb %%al,(%1)";
    register_store_type = C_U8;
    register_store_flags = NYIR_INST_F_MEM_BYTE;
  } else if (assembly_len == sizeof("movw %%ax,(%1)") - 1 &&
             memcmp(assembly, "movw %%ax,(%1)", assembly_len) == 0) {
    register_store_template = "movw %%ax,(%1)";
    register_store_type = C_U16;
    register_store_flags = NYIR_INST_F_MEM_U16;
  } else if (assembly_len == sizeof("movl %%eax,(%1)") - 1 &&
             memcmp(assembly, "movl %%eax,(%1)", assembly_len) == 0) {
    register_store_template = "movl %%eax,(%1)";
    register_store_type = C_U32;
    register_store_flags = NYIR_INST_F_MEM_I32;
  }
  if (register_store_template) {
    if (!expect(p, ":") || !expect(p, ":") ||
        !inline_asm_string_sequence_is(p, "a", &p->pos) || !expect(p, "("))
      return defer_unsupported_asm(p, open, assembly);
    c_value_t value = parse_expr(p, 1);
    if (value.value < 0 || value.type != register_store_type ||
        !expect(p, ")") || !expect(p, ",") ||
        !inline_asm_string_sequence_is(p, "r", &p->pos) || !expect(p, "("))
      return defer_unsupported_asm(p, open, assembly);
    c_value_t pointer = parse_expr(p, 1);
    if (!expect(p, ")"))
      return defer_unsupported_asm(p, open, assembly);
    if (accept(p, ":")) {
      size_t clobber_end = p->pos;
      if (!inline_asm_clobber_list_is(p, &clobber_end))
        return defer_unsupported_asm(p, open, assembly);
      p->pos = clobber_end;
    }
    if (!expect(p, ")") || !expect(p, ";"))
      return defer_unsupported_asm(p, open, assembly);
    if (pointer.value < 0 || !c_type_is_pointer(pointer.type)) {
      free(assembly);
      fail(p, "unsupported register-addressed store inline-assembly operand");
      return false;
    }
    bool ok = emit(p,
                   (nyr_inst_t){.op = NYIR_STORE_I64,
                                .dst = -1,
                                .a = pointer.value,
                                .c = value.value,
                                .effects = NYIR_EFFECT_WRITE_MEMORY |
                                           NYIR_EFFECT_MAY_TRAP |
                                           NYIR_EFFECT_VOLATILE,
                                .flags = register_store_flags,
                                .scalar_class = NYIR_SCALAR_I64_RAW},
                   NULL);
    free(assembly);
    return ok;
  }
  if (assembly_len == sizeof("movq %1,%0") - 1 &&
      memcmp(assembly, "movq %1,%0", assembly_len) == 0) {
    size_t constraint_end = p->pos;
    ny_ctok_t output_name;
    if (!expect(p, ":") ||
        !inline_asm_string_sequence_is(p, "=r", &constraint_end))
      return defer_unsupported_asm(p, open, assembly);
    p->pos = constraint_end;
    if (!expect(p, "(") || !parse_name(p, &output_name) || !expect(p, ")") ||
        !expect(p, ":") || !inline_asm_string_sequence_is(p, "m", &p->pos) ||
        !expect(p, "("))
      return defer_unsupported_asm(p, open, assembly);
    c_value_t source = parse_expr(p, 1);
    const c_lvalue_t *source_lvalue =
        source.value >= 0 ? find_asm_memory_lvalue(p, source.value) : NULL;
    if (!source_lvalue || source_lvalue->address < 0 ||
        (source_lvalue->type != C_I64 && source_lvalue->type != C_U64) ||
        !expect(p, ")"))
      return defer_unsupported_asm(p, open, assembly);
    if (accept(p, ":")) {
      size_t clobber_end = p->pos;
      if (!inline_asm_clobber_list_is(p, &clobber_end))
        return defer_unsupported_asm(p, open, assembly);
      p->pos = clobber_end;
    }
    if (!expect(p, ")") || !expect(p, ";"))
      return defer_unsupported_asm(p, open, assembly);
    c_local_t *output = find_local(p, output_name);
    if (!output || output->array_rank || output->is_const ||
        (output->type != C_I64 && output->type != C_U64)) {
      free(assembly);
      fail(p, "unsupported movq inline-assembly output");
      return false;
    }
    int loaded = lower_load_field(p, source_lvalue->address, false, true);
    bool ok = loaded >= 0 && lower_local_store(p, output, loaded);
    free(assembly);
    return ok;
  }
  if (assembly_len == sizeof("movl %0,%1") - 1 &&
      memcmp(assembly, "movl %0,%1", assembly_len) == 0) {
    size_t constraint_end = p->pos;
    if (!expect(p, ":") || !expect(p, ":") ||
        !inline_asm_string_sequence_is(p, "r", &constraint_end))
      return defer_unsupported_asm(p, open, assembly);
    p->pos = constraint_end;
    if (!expect(p, "("))
      return defer_unsupported_asm(p, open, assembly);
    c_value_t value = parse_expr(p, 1);
    if (value.value < 0 || !c_type_is_integer(value.type) || !expect(p, ")") ||
        !expect(p, ",") || !inline_asm_string_sequence_is(p, "m", &p->pos) ||
        !expect(p, "("))
      return defer_unsupported_asm(p, open, assembly);
    c_value_t target = parse_expr(p, 1);
    const c_lvalue_t *target_lvalue =
        target.value >= 0 ? find_lvalue(p, target.value) : NULL;
    if (!target_lvalue || target_lvalue->address < 0 ||
        (target_lvalue->type != C_I32 && target_lvalue->type != C_U32) ||
        !expect(p, ")"))
      return defer_unsupported_asm(p, open, assembly);
    if (accept(p, ":")) {
      size_t clobber_end = p->pos;
      if (!inline_asm_clobber_list_is(p, &clobber_end))
        return defer_unsupported_asm(p, open, assembly);
      p->pos = clobber_end;
    }
    if (!expect(p, ")") || !expect(p, ";"))
      return defer_unsupported_asm(p, open, assembly);
    int stored_value = normalize_integer(p, target_lvalue->type, value.value);
    bool ok =
        stored_value >= 0 &&
        lower_store_i32_field(p, target_lvalue->address, stored_value, true);
    free(assembly);
    return ok;
  }
  if (inline_asm_template_is(assembly, assembly_len, "btsl %1,%0")) {
    atomic_memory_rmw_mnemonic = "btsl";
    atomic_memory_rmw_locked = false;
  } else if (inline_asm_template_is(assembly, assembly_len, "btrl %1,%0")) {
    atomic_memory_rmw_mnemonic = "btrl";
    atomic_memory_rmw_locked = false;
  } else if (inline_asm_template_is(assembly, assembly_len, "btcl %1,%0")) {
    atomic_memory_rmw_mnemonic = "btcl";
    atomic_memory_rmw_locked = false;
  } else if (inline_asm_template_is(assembly, assembly_len, "lock btsl %1,%0"))
    atomic_memory_rmw_mnemonic = "btsl";
  else if (inline_asm_template_is(assembly, assembly_len, "lock btrl %1,%0"))
    atomic_memory_rmw_mnemonic = "btrl";
  else if (inline_asm_template_is(assembly, assembly_len, "lock btcl %1,%0"))
    atomic_memory_rmw_mnemonic = "btcl";
  else if (assembly_len == sizeof("lock andl %1, %0") - 1 &&
           memcmp(assembly, "lock andl %1, %0", assembly_len) == 0)
    atomic_memory_rmw_mnemonic = "andl";
  else if (assembly_len == sizeof("lock orl %1, %0") - 1 &&
           memcmp(assembly, "lock orl %1, %0", assembly_len) == 0)
    atomic_memory_rmw_mnemonic = "orl";
  else if (assembly_len == sizeof("lock xorl %1, %0") - 1 &&
           memcmp(assembly, "lock xorl %1, %0", assembly_len) == 0)
    atomic_memory_rmw_mnemonic = "xorl";
  else if (assembly_len == sizeof("lock addl %1, %0") - 1 &&
           memcmp(assembly, "lock addl %1, %0", assembly_len) == 0)
    atomic_memory_rmw_mnemonic = "addl";
  else if (assembly_len == sizeof("lock subl %1, %0") - 1 &&
           memcmp(assembly, "lock subl %1, %0", assembly_len) == 0)
    atomic_memory_rmw_mnemonic = "subl";
  else if (assembly_len == sizeof("lock andq %1, %0") - 1 &&
           memcmp(assembly, "lock andq %1, %0", assembly_len) == 0)
    atomic_memory_rmw_mnemonic = "andq";
  else if (assembly_len == sizeof("lock orq %1, %0") - 1 &&
           memcmp(assembly, "lock orq %1, %0", assembly_len) == 0)
    atomic_memory_rmw_mnemonic = "orq";
  else if (assembly_len == sizeof("lock xorq %1, %0") - 1 &&
           memcmp(assembly, "lock xorq %1, %0", assembly_len) == 0)
    atomic_memory_rmw_mnemonic = "xorq";
  else if (assembly_len == sizeof("lock addq %1, %0") - 1 &&
           memcmp(assembly, "lock addq %1, %0", assembly_len) == 0)
    atomic_memory_rmw_mnemonic = "addq";
  else if (assembly_len == sizeof("lock subq %1, %0") - 1 &&
           memcmp(assembly, "lock subq %1, %0", assembly_len) == 0)
    atomic_memory_rmw_mnemonic = "subq";
  else if (assembly_len == sizeof("lock andb %b1,%0") - 1 &&
           memcmp(assembly, "lock andb %b1,%0", assembly_len) == 0)
    atomic_memory_rmw_mnemonic = "andb";
  else if (assembly_len == sizeof("lock orb %b1,%0") - 1 &&
           memcmp(assembly, "lock orb %b1,%0", assembly_len) == 0)
    atomic_memory_rmw_mnemonic = "orb";
  else if (assembly_len == sizeof("lock xorb %b1,%0") - 1 &&
           memcmp(assembly, "lock xorb %b1,%0", assembly_len) == 0)
    atomic_memory_rmw_mnemonic = "xorb";
  bool atomic_xorb_condition =
      assembly_len == sizeof("lock xorb %2,%1") - 1 &&
      memcmp(assembly, "lock xorb %2,%1", assembly_len) == 0;
  bool atomic_bts_condition =
      assembly_len == sizeof("lock btsl %[val], %[var]") - 1 &&
      memcmp(assembly, "lock btsl %[val], %[var]", assembly_len) == 0;
  bool bt_condition = false;
  const char *bt_mnemonic = NULL;
  bool test_nz_condition = false;
  if (assembly_len == sizeof("btq %2,%1") - 1 &&
      memcmp(assembly, "btq %2,%1", assembly_len) == 0) {
    bt_condition = true;
    bt_mnemonic = "btq";
  } else if (assembly_len == sizeof("btl %2,%1") - 1 &&
             memcmp(assembly, "btl %2,%1", assembly_len) == 0) {
    bt_condition = true;
    bt_mnemonic = "btl";
  } else if (inline_asm_template_is(assembly, assembly_len, "btq %2,%1")) {
    bt_condition = true;
    bt_mnemonic = "btq";
  } else if (inline_asm_template_is(assembly, assembly_len, "btl %2,%1")) {
    bt_condition = true;
    bt_mnemonic = "btl";
  } else if (inline_asm_template_is(assembly, assembly_len, "testb %2,%1") ||
             (assembly_len == sizeof("testb %2,%1") - 1 &&
              memcmp(assembly, "testb %2,%1", assembly_len) == 0)) {
    test_nz_condition = true;
  }
  if (inline_asm_template_is(assembly, assembly_len,
                             "lock addl %[val], %[var]"))
    atomic_arithmetic_mnemonic = "addl";
  else if (inline_asm_template_is(assembly, assembly_len,
                                  "lock subl %[val], %[var]"))
    atomic_arithmetic_mnemonic = "subl";
  else if (inline_asm_template_is(assembly, assembly_len,
                                  "lock addq %[val], %[var]"))
    atomic_arithmetic_mnemonic = "addq";
  else if (inline_asm_template_is(assembly, assembly_len,
                                  "lock subq %[val], %[var]"))
    atomic_arithmetic_mnemonic = "subq";
  else if (inline_asm_template_is(assembly, assembly_len,
                                  "addl %[val], %[var]")) {
    atomic_arithmetic_mnemonic = "addl";
    atomic_arithmetic_locked = false;
  } else if (inline_asm_template_is(assembly, assembly_len,
                                    "subl %[val], %[var]")) {
    atomic_arithmetic_mnemonic = "subl";
    atomic_arithmetic_locked = false;
  } else if (inline_asm_template_is(assembly, assembly_len,
                                    "addq %[val], %[var]")) {
    atomic_arithmetic_mnemonic = "addq";
    atomic_arithmetic_locked = false;
  } else if (inline_asm_template_is(assembly, assembly_len,
                                    "subq %[val], %[var]")) {
    atomic_arithmetic_mnemonic = "subq";
    atomic_arithmetic_locked = false;
  }
  char gs_add_width = 0;
  if (inline_asm_template_is(assembly, assembly_len, "addb %[val], %[var]"))
    gs_add_width = 'b';
  else if (inline_asm_template_is(assembly, assembly_len,
                                  "addw %[val], %[var]"))
    gs_add_width = 'w';
  else if (inline_asm_template_is(assembly, assembly_len,
                                  "addl %[val], %[var]"))
    gs_add_width = 'l';
  else if (inline_asm_template_is(assembly, assembly_len,
                                  "addq %[val], %[var]"))
    gs_add_width = 'q';
  bool segmented_gs_add =
      gs_add_width != 0 && inline_asm_statement_has_token(p, open, "__seg_gs");
  if (segmented_gs_add && accept(p, ":")) {
    ny_ctok_t output_name, input_name;
    size_t constraint_end = p->pos;
    if (!parse_named_inline_asm_operand(p, &output_name) ||
        !inline_asm_name_matches(output_name, (const unsigned char *)"var",
                                 3) ||
        !inline_asm_string_sequence_is(p, "+m", &constraint_end)) {
      fail(p, "invalid GS-relative add output constraint");
      return false;
    }
    p->pos = constraint_end;
    if (!expect(p, "(")) {
      fail(p, "invalid GS-relative add output operand");
      return false;
    }
    size_t output_start = p->pos;
    c_value_t output = parse_expr(p, 1);
    size_t output_end = p->pos;
    const c_lvalue_t *memory =
        output.value >= 0 ? find_lvalue(p, output.value) : NULL;
    bool gs_address = false;
    for (size_t i = output_start; i < output_end; ++i)
      gs_address |= tok_is(p, i, "__seg_gs");
    if (!gs_address || !memory || memory->address < 0 ||
        !c_type_is_integer(memory->type)) {
      fail(p, "GS-relative add requires an integer GS-qualified memory lvalue");
      return false;
    }
    if (!expect(p, ")") || !discard_lvalue_read(p, memory) || !expect(p, ":") ||
        !parse_named_inline_asm_operand(p, &input_name) ||
        !inline_asm_name_matches(input_name, (const unsigned char *)"val", 3)) {
      fail(p, "invalid GS-relative add input operand");
      return false;
    }
    constraint_end = p->pos;
    const char *input_constraint = gs_add_width == 'b'   ? "qi"
                                   : gs_add_width == 'q' ? "re"
                                                         : "ri";
    if (!inline_asm_string_sequence_is(p, input_constraint, &constraint_end)) {
      fail(p, "invalid GS-relative add input constraint");
      return false;
    }
    p->pos = constraint_end;
    if (!expect(p, "(")) {
      fail(p, "invalid GS-relative add input operand");
      return false;
    }
    c_value_t input = parse_expr(p, 1);
    if (input.value < 0 || !c_type_is_integer(input.type) || !expect(p, ")")) {
      fail(p, "GS-relative add input must be an integer expression");
      return false;
    }
    if (accept(p, ":")) {
      size_t clobber_end = p->pos;
      if (!inline_asm_clobber_list_is(p, &clobber_end)) {
        fail(p, "unsupported GS-relative add clobber list");
        return false;
      }
      p->pos = clobber_end;
    }
    if (!expect(p, ")") || !expect(p, ";")) {
      fail(p, "invalid GS-relative add statement terminator");
      return false;
    }
    bool ok = emit(p,
                   (nyr_inst_t){.op = NYIR_INLINE_ASM,
                                .dst = -1,
                                .a = memory->address,
                                .b = input.value,
                                .imm = -181,
                                .effects = NYIR_EFFECT_READ_MEMORY |
                                           NYIR_EFFECT_WRITE_MEMORY |
                                           NYIR_EFFECT_VOLATILE,
                                .symbol = gs_add_width == 'b'   ? "gs_addb"
                                          : gs_add_width == 'w' ? "gs_addw"
                                          : gs_add_width == 'l' ? "gs_addl"
                                                                : "gs_addq"},
                   NULL);
    free(assembly);
    return ok;
  }
  if (atomic_arithmetic_mnemonic && accept(p, ":")) {
#define ATOMIC_ARITHMETIC_DEFER() defer_unsupported_asm(p, open, assembly)
    size_t constraint_end = p->pos;
    ny_ctok_t condition_name;
    if (!parse_inline_asm_operand_name(p) ||
        !inline_asm_string_sequence_is(p, "+m", &constraint_end))
      return ATOMIC_ARITHMETIC_DEFER();
    p->pos = constraint_end;
    if (!expect(p, "("))
      return ATOMIC_ARITHMETIC_DEFER();
    c_value_t output = parse_expr(p, 1);
    const c_lvalue_t *memory =
        output.value >= 0 ? find_lvalue(p, output.value) : NULL;
    bool width64 = atomic_arithmetic_mnemonic[3] == 'q';
    if (!memory || memory->address < 0 ||
        (width64 ? (memory->type != C_I64 && memory->type != C_U64)
                 : (memory->type != C_I32 && memory->type != C_U32)) ||
        !expect(p, ")") || !discard_lvalue_read(p, memory) || !expect(p, ",") ||
        !parse_inline_asm_operand_name(p))
      return ATOMIC_ARITHMETIC_DEFER();
    bool zero_condition = false;
    size_t condition_constraint_end = p->pos;
    if (!inline_asm_string_sequence_is(p, "=@cce", &condition_constraint_end)) {
      if (!inline_asm_string_sequence_is(p, "=@ccs", &condition_constraint_end))
        return ATOMIC_ARITHMETIC_DEFER();
    } else {
      zero_condition = true;
    }
    p->pos = condition_constraint_end;
    if (!expect(p, "(") || !parse_name(p, &condition_name) || !expect(p, ")") ||
        !expect(p, ":") || !parse_inline_asm_operand_name(p) ||
        !((inline_asm_string_sequence_is(p, "er", &p->pos) || ({
             size_t _e = p->pos;
             bool _m = inline_asm_string_sequence_is(p, "re", &_e) ||
                       inline_asm_string_sequence_is(p, "ir", &_e) ||
                       inline_asm_string_sequence_is(p, "ri", &_e) ||
                       inline_asm_string_sequence_is(p, "r", &_e) ||
                       inline_asm_string_sequence_is(p, "i", &_e) ||
                       inline_asm_string_sequence_is(p, "n", &_e);
             if (_m)
               p->pos = _e;
             _m;
           }))) ||
        !expect(p, "("))
      return ATOMIC_ARITHMETIC_DEFER();
    c_value_t input = parse_expr(p, 1);
    c_local_t *condition_local = find_local(p, condition_name);
    if (input.value < 0 || !c_type_is_integer(input.type) || !expect(p, ")") ||
        !expect(p, ":"))
      return ATOMIC_ARITHMETIC_DEFER();
    size_t clobber_end = p->pos;
    if (!inline_asm_clobber_list_is(p, &clobber_end))
      return ATOMIC_ARITHMETIC_DEFER();
    p->pos = clobber_end;
    if (!condition_local || condition_local->type != C_U8 ||
        condition_local->is_const || condition_local->array_rank)
      return defer_unsupported_asm(p, open, assembly);
    asm_condition_output = condition_local;
    asm_memory_output_value = memory->address;
    asm_memory_register_input_value = input.value;
    if (zero_condition) {
      static const char *const zero_mnemonics[] = {"addl_zero", "subl_zero",
                                                   "addq_zero", "subq_zero"};
      size_t index = strcmp(atomic_arithmetic_mnemonic, "addl") == 0   ? 0
                     : strcmp(atomic_arithmetic_mnemonic, "subl") == 0 ? 1
                     : strcmp(atomic_arithmetic_mnemonic, "addq") == 0 ? 2
                                                                       : 3;
      atomic_arithmetic_mnemonic = zero_mnemonics[index];
    }
    if (!atomic_arithmetic_locked) {
      static const char *const local_mnemonics[] = {
          "addl_unlocked", "subl_unlocked", "addq_unlocked", "subq_unlocked"};
      size_t index = strcmp(atomic_arithmetic_mnemonic, "addl") == 0        ? 0
                     : strcmp(atomic_arithmetic_mnemonic, "subl") == 0      ? 1
                     : strcmp(atomic_arithmetic_mnemonic, "addq") == 0      ? 2
                     : strcmp(atomic_arithmetic_mnemonic, "subq") == 0      ? 3
                     : strcmp(atomic_arithmetic_mnemonic, "addl_zero") == 0 ? 0
                     : strcmp(atomic_arithmetic_mnemonic, "subl_zero") == 0 ? 1
                     : strcmp(atomic_arithmetic_mnemonic, "addq_zero") == 0 ? 2
                                                                            : 3;
      static const char *const local_zero_mnemonics[] = {
          "addl_unlocked_zero", "subl_unlocked_zero", "addq_unlocked_zero",
          "subq_unlocked_zero"};
      atomic_arithmetic_mnemonic =
          zero_condition ? local_zero_mnemonics[index] : local_mnemonics[index];
    }
    lower_atomic_arithmetic_condition = true;
#undef ATOMIC_ARITHMETIC_DEFER
  } else if (test_nz_condition && accept(p, ":")) {
    ny_ctok_t condition_name;
    if (!inline_asm_string_sequence_is(p, "=@ccnz", &p->pos) ||
        !expect(p, "(") || !parse_name(p, &condition_name) || !expect(p, ")") ||
        !expect(p, ":") || !inline_asm_string_sequence_is(p, "m", &p->pos) ||
        !expect(p, "("))
      return defer_unsupported_asm(p, open, assembly);
    c_value_t mem = parse_expr(p, 1);
    const c_lvalue_t *memory =
        mem.value >= 0 ? find_lvalue(p, mem.value) : NULL;
    if (!memory || memory->address < 0 || !expect(p, ")") || !expect(p, ","))
      return defer_unsupported_asm(p, open, assembly);
    size_t ce = p->pos;
    if (!inline_asm_string_sequence_is(p, "i", &ce) &&
        !inline_asm_string_sequence_is(p, "n", &ce) &&
        !inline_asm_string_sequence_is(p, "Ir", &ce) &&
        !inline_asm_string_sequence_is(p, "r", &ce) &&
        !inline_asm_string_sequence_is(p, "I", &ce))
      return defer_unsupported_asm(p, open, assembly);
    p->pos = ce;
    if (!expect(p, "("))
      return defer_unsupported_asm(p, open, assembly);
    c_value_t input = parse_expr(p, 1);
    if (input.value < 0 || !c_type_is_integer(input.type) || !expect(p, ")") ||
        !expect(p, ":"))
      return defer_unsupported_asm(p, open, assembly);
    size_t clobber_end = p->pos;
    if (!inline_asm_clobber_list_is(p, &clobber_end))
      return defer_unsupported_asm(p, open, assembly);
    p->pos = clobber_end;
    asm_condition_output = find_local(p, condition_name);
    if (!asm_condition_output || asm_condition_output->type != C_U8 ||
        asm_condition_output->is_const || asm_condition_output->array_rank)
      return defer_unsupported_asm(p, open, assembly);
    asm_memory_output_value = memory->address;
    asm_memory_register_input_value = input.value;
    bt_condition_symbol = "testb_nz";
    lower_bt_condition = true;
  } else if (bt_condition && accept(p, ":")) {
    /*
     * bt{l,q} %2,%1 with =@ccc output, m memory input, Ir/r index
     */
    ny_ctok_t condition_name;
    if (!inline_asm_string_sequence_is(p, "=@ccc", &p->pos) ||
        !expect(p, "(") || !parse_name(p, &condition_name) || !expect(p, ")") ||
        !expect(p, ":") || !inline_asm_string_sequence_is(p, "m", &p->pos) ||
        !expect(p, "("))
      return defer_unsupported_asm(p, open, assembly);
    c_value_t mem = parse_expr(p, 1);
    const c_lvalue_t *memory =
        mem.value >= 0 ? find_lvalue(p, mem.value) : NULL;
    if (!memory || memory->address < 0 || !expect(p, ")") ||
        !discard_lvalue_read(p, memory) || !expect(p, ","))
      return defer_unsupported_asm(p, open, assembly);
    size_t ce = p->pos;
    bool ir = inline_asm_string_sequence_is(p, "Ir", &ce) ||
              inline_asm_string_sequence_is(p, "rI", &ce) ||
              inline_asm_string_sequence_is(p, "r", &ce) ||
              inline_asm_string_sequence_is(p, "I", &ce) ||
              inline_asm_string_sequence_is(p, "i", &ce) ||
              inline_asm_string_sequence_is(p, "n", &ce);
    if (!ir)
      return defer_unsupported_asm(p, open, assembly);
    p->pos = ce;
    if (!expect(p, "("))
      return defer_unsupported_asm(p, open, assembly);
    c_value_t input = parse_expr(p, 1);
    if (input.value < 0 || !c_type_is_integer(input.type) || !expect(p, ")"))
      return defer_unsupported_asm(p, open, assembly);
    if (accept(p, ":")) {
      size_t clobber_end = p->pos;
      if (!inline_asm_clobber_list_is(p, &clobber_end))
        return defer_unsupported_asm(p, open, assembly);
      p->pos = clobber_end;
    }
    asm_condition_output = find_local(p, condition_name);
    if (!asm_condition_output || asm_condition_output->type != C_U8 ||
        asm_condition_output->is_const || asm_condition_output->array_rank)
      return defer_unsupported_asm(p, open, assembly);
    asm_memory_output_value = memory->address;
    asm_memory_register_input_value = input.value;
    bt_condition_symbol = bt_mnemonic;
    lower_bt_condition = true;
  } else if (atomic_bts_condition && accept(p, ":")) {
    size_t constraint_end = p->pos;
    if (!parse_inline_asm_operand_name(p) ||
        !inline_asm_string_sequence_is(p, "+m", &constraint_end))
      return defer_unsupported_asm(p, open, assembly);
    p->pos = constraint_end;
    if (!expect(p, "("))
      return defer_unsupported_asm(p, open, assembly);
    c_value_t output = parse_expr(p, 1);
    const c_lvalue_t *memory =
        output.value >= 0 ? find_lvalue(p, output.value) : NULL;
    if (!memory || memory->address < 0 ||
        (memory->type != C_I32 && memory->type != C_U32) || !expect(p, ")") ||
        !discard_lvalue_read(p, memory) || !expect(p, ",") ||
        !parse_inline_asm_operand_name(p))
      return defer_unsupported_asm(p, open, assembly);
    ny_ctok_t condition_name;
    constraint_end = p->pos;
    if (!inline_asm_string_sequence_is(p, "=@ccc", &constraint_end))
      return defer_unsupported_asm(p, open, assembly);
    p->pos = constraint_end;
    if (!expect(p, "(") || !parse_name(p, &condition_name) || !expect(p, ")") ||
        !expect(p, ":") || !parse_inline_asm_operand_name(p) ||
        !inline_asm_string_sequence_is(p, "I", &p->pos) || !expect(p, "("))
      return defer_unsupported_asm(p, open, assembly);
    size_t input_start = p->pos;
    c_value_t input = parse_expr(p, 1);
    int64_t bit_index = -1;
    size_t constant_pos = input_start;
    c_parser_t constant_probe = *p;
    if (input.value < 0 || !c_type_is_integer(input.type) || !expect(p, ")") ||
        !parse_global_const_expr(&constant_probe, &constant_pos, &bit_index) ||
        constant_pos != p->pos - 1 || bit_index > 31 || !expect(p, ":"))
      return defer_unsupported_asm(p, open, assembly);
    size_t clobber_end = p->pos;
    if (!inline_asm_clobber_list_is(p, &clobber_end))
      return defer_unsupported_asm(p, open, assembly);
    p->pos = clobber_end;
    asm_condition_output = find_local(p, condition_name);
    if (!asm_condition_output || asm_condition_output->type != C_U8 ||
        asm_condition_output->is_const || asm_condition_output->array_rank)
      return defer_unsupported_asm(p, open, assembly);
    asm_memory_output_value = memory->address;
    asm_memory_register_input_value = input.value;
    lower_atomic_bts_condition = true;
  } else if (atomic_xorb_condition && accept(p, ":")) {
    size_t constraint_end = p->pos;
    ny_ctok_t condition_name;
    if (!inline_asm_string_sequence_is(p, "=@ccs", &constraint_end))
      return defer_unsupported_asm(p, open, assembly);
    p->pos = constraint_end;
    if (!expect(p, "(") || !parse_name(p, &condition_name) || !expect(p, ")") ||
        !expect(p, ",") || !inline_asm_string_sequence_is(p, "+m", &p->pos) ||
        !expect(p, "("))
      return defer_unsupported_asm(p, open, assembly);
    c_value_t output = parse_expr(p, 1);
    const c_lvalue_t *out_lv =
        output.value >= 0 ? find_lvalue(p, output.value) : NULL;
    if (!out_lv || out_lv->address < 0 ||
        (out_lv->type != C_CHAR && out_lv->type != C_U8) || !expect(p, ")") ||
        !discard_lvalue_read(p, out_lv) || !expect(p, ":") ||
        !inline_asm_string_sequence_is(p, "iq", &p->pos) || !expect(p, "("))
      return defer_unsupported_asm(p, open, assembly);
    c_value_t input = parse_expr(p, 1);
    if (input.value < 0 || !c_type_is_integer(input.type) || !expect(p, ")") ||
        !expect(p, ":"))
      return defer_unsupported_asm(p, open, assembly);
    size_t clobber_end = p->pos;
    if (!inline_asm_clobber_list_is(p, &clobber_end))
      return defer_unsupported_asm(p, open, assembly);
    p->pos = clobber_end;
    asm_condition_output = find_local(p, condition_name);
    if (!asm_condition_output || asm_condition_output->type != C_U8 ||
        asm_condition_output->is_const || asm_condition_output->array_rank)
      return defer_unsupported_asm(p, open, assembly);
    asm_memory_output_value = out_lv->address;
    asm_memory_register_input_value = input.value;
    lower_atomic_bitwise_condition = true;
  } else if (atomic_memory_rmw_mnemonic && accept(p, ":")) {
    size_t constraint_end = p->pos;
    bool read_write_output =
        inline_asm_string_sequence_is(p, "+m", &constraint_end);
    if (!read_write_output &&
        !inline_asm_string_sequence_is(p, "=m", &constraint_end))
      return defer_unsupported_asm(p, open, assembly);
    p->pos = constraint_end;
    if (!expect(p, "("))
      return defer_unsupported_asm(p, open, assembly);
    c_value_t output = parse_expr(p, 1);
    const c_lvalue_t *out_lv =
        output.value >= 0 ? find_lvalue(p, output.value) : NULL;
    size_t mnemonic_len = strlen(atomic_memory_rmw_mnemonic);
    bool width64 =
        mnemonic_len && atomic_memory_rmw_mnemonic[mnemonic_len - 1] == 'q';
    bool width8 =
        mnemonic_len && atomic_memory_rmw_mnemonic[mnemonic_len - 1] == 'b';
    if (!out_lv || out_lv->address < 0 ||
        (width64  ? (out_lv->type != C_I64 && out_lv->type != C_U64)
         : width8 ? !(c_type_is_integer(out_lv->type) ||
                      c_type_is_pointer(out_lv->type))
                  : (out_lv->type != C_I32 && out_lv->type != C_U32)) ||
        !expect(p, ")") ||
        (read_write_output && !discard_lvalue_read(p, out_lv)) ||
        !expect(p, ":"))
      return defer_unsupported_asm(p, open, assembly);
    constraint_end = p->pos;
    if (!inline_asm_string_sequence_is(p, "ir", &constraint_end) &&
        !inline_asm_string_sequence_is(p, "Ir", &constraint_end) &&
        !inline_asm_string_sequence_is(p, "er", &constraint_end) &&
        !inline_asm_string_sequence_is(p, "iq", &constraint_end) &&
        !inline_asm_string_sequence_is(p, "ri", &constraint_end) &&
        !inline_asm_string_sequence_is(p, "i", &constraint_end) &&
        !inline_asm_string_sequence_is(p, "n", &constraint_end) &&
        !inline_asm_string_sequence_is(p, "r", &constraint_end))
      return defer_unsupported_asm(p, open, assembly);
    p->pos = constraint_end;
    if (!expect(p, "("))
      return defer_unsupported_asm(p, open, assembly);
    c_value_t input = parse_expr(p, 1);
    if (input.value < 0 || !c_type_is_integer(input.type) || !expect(p, ")"))
      return defer_unsupported_asm(p, open, assembly);
    if (!read_write_output) {
      if (!expect(p, ","))
        return defer_unsupported_asm(p, open, assembly);
      constraint_end = p->pos;
      if (!inline_asm_string_sequence_is(p, "m", &constraint_end))
        return defer_unsupported_asm(p, open, assembly);
      p->pos = constraint_end;
      if (!expect(p, "("))
        return defer_unsupported_asm(p, open, assembly);
      c_value_t memory_input = parse_expr(p, 1);
      const c_lvalue_t *input_lvalue =
          memory_input.value >= 0 ? find_lvalue(p, memory_input.value) : NULL;
      if (!input_lvalue || input_lvalue->address < 0 ||
          (width64
               ? (input_lvalue->type != C_I64 && input_lvalue->type != C_U64)
               : (input_lvalue->type != C_I32 &&
                  input_lvalue->type != C_U32)) ||
          !expect(p, ")"))
        return defer_unsupported_asm(p, open, assembly);
    }
    if (accept(p, ":")) {
      size_t clobber_end = p->pos;
      if (!inline_asm_clobber_list_is(p, &clobber_end))
        return defer_unsupported_asm(p, open, assembly);
      p->pos = clobber_end;
    }
    asm_memory_output_value = out_lv->address;
    asm_memory_register_input_value = input.value;
    lower_atomic_memory_rmw64 = width64;
    lower_atomic_memory_rmw = !width64;
  } else if (((assembly_len == sizeof("lock incq %0") - 1 &&
               memcmp(assembly, "lock incq %0", assembly_len) == 0) ||
              (assembly_len == sizeof("lock decq %0") - 1 &&
               memcmp(assembly, "lock decq %0", assembly_len) == 0)) &&
             accept(p, ":")) {
    bool decrement = assembly[5] == 'd';
    size_t constraint_end = p->pos;
    if (!inline_asm_string_sequence_is(p, "=m", &constraint_end))
      return defer_unsupported_asm(p, open, assembly);
    p->pos = constraint_end;
    if (!expect(p, "("))
      return defer_unsupported_asm(p, open, assembly);
    c_value_t output = parse_expr(p, 1);
    const c_lvalue_t *out_lv =
        output.value >= 0 ? find_lvalue(p, output.value) : NULL;
    if (!out_lv || out_lv->address < 0 || !expect(p, ")") || !expect(p, ":"))
      return defer_unsupported_asm(p, open, assembly);
    int output_address = out_lv->address;
    constraint_end = p->pos;
    if (!inline_asm_string_sequence_is(p, "m", &constraint_end))
      return defer_unsupported_asm(p, open, assembly);
    p->pos = constraint_end;
    if (!expect(p, "("))
      return defer_unsupported_asm(p, open, assembly);
    c_value_t input = parse_expr(p, 1);
    const c_lvalue_t *in_lv =
        input.value >= 0 ? find_lvalue(p, input.value) : NULL;
    if (!in_lv || in_lv->address < 0 || !expect(p, ")") || !expect(p, ":"))
      return defer_unsupported_asm(p, open, assembly);
    size_t clobber_end = p->pos;
    if (!inline_asm_clobber_list_is(p, &clobber_end))
      return defer_unsupported_asm(p, open, assembly);
    p->pos = clobber_end;
    asm_memory_output_value = output_address;
    lower_atomic_inc = !decrement;
    lower_atomic_dec = decrement;
  } else if (((assembly_len == sizeof("lock cmpxchg16b %[ptr]") - 1 &&
               memcmp(assembly, "lock cmpxchg16b %[ptr]", assembly_len) == 0) ||
              (assembly_len == sizeof("cmpxchg16b %[ptr]") - 1 &&
               memcmp(assembly, "cmpxchg16b %[ptr]", assembly_len) == 0)) &&
             accept(p, ":")) {
    bool locked = assembly[0] == 'l';
    ny_ctok_t condition_name = {0}, memory_name;
    size_t condition_end = p->pos;
    if (tok_is(p, condition_end, "["))
      condition_end = skip_balanced(p, condition_end, "[", "]");
    if (inline_asm_string_sequence_is(p, "=@ccz", &condition_end) &&
        (!parse_inline_asm_operand_name(p) ||
         !inline_asm_string_sequence_is(p, "=@ccz", &p->pos) ||
         !expect(p, "(") || !parse_name(p, &condition_name) ||
         !expect(p, ")") || !expect(p, ",")))
      return defer_unsupported_asm(p, open, assembly);
    if (!parse_named_inline_asm_operand(p, &memory_name) ||
        !inline_asm_name_matches(memory_name, (const unsigned char *)"ptr",
                                 3) ||
        !inline_asm_string_sequence_is(p, "+m", &p->pos) || !expect(p, "("))
      return defer_unsupported_asm(p, open, assembly);
    c_value_t memory = parse_expr(p, 1);
    const c_lvalue_t *memory_lvalue =
        memory.value >= 0 ? find_lvalue(p, memory.value) : NULL;
    if (!memory_lvalue || memory.type != C_AGG ||
        (memory_lvalue->type != C_AGG && memory_lvalue->type != C_AGG_PTR) ||
        !memory_lvalue->agg || memory_lvalue->agg->size != 16)
      return defer_unsupported_asm(p, open, assembly);
    if (!expect(p, ")"))
      return defer_unsupported_asm(p, open, assembly);
    if (!expect(p, ","))
      return defer_unsupported_asm(p, open, assembly);
    int memory_address = memory_lvalue->address >= 0 ? memory_lvalue->address
                         : memory.type == C_AGG      ? memory.value
                                                     : -1;
    if (memory_address < 0)
      return defer_unsupported_asm(p, open, assembly);

    const char *output_constraints[2] = {"+a", "+d"};
    c_local_t *old_object = NULL;
    for (size_t i = 0; i < 2; ++i) {
      if (!parse_inline_asm_operand_name(p) ||
          !inline_asm_string_sequence_is(p, output_constraints[i], &p->pos) ||
          !expect(p, "("))
        return defer_unsupported_asm(p, open, assembly);
      size_t expression_start = p->pos;
      c_value_t field_value = parse_expr(p, 1);
      const c_lvalue_t *field_lvalue =
          field_value.value >= 0 ? find_lvalue(p, field_value.value) : NULL;
      if (!field_lvalue || field_lvalue->type != C_U64 ||
          expression_start + 2 >= p->token_count ||
          !tok_ident(p->tokens[expression_start]) ||
          !tok_is(p, expression_start + 1, "."))
        return defer_unsupported_asm(p, open, assembly);
      c_local_t *object = find_local(p, p->tokens[expression_start]);
      const c_agg_field_t *field =
          object && object->type == C_AGG
              ? find_field(object->agg, p->tokens[expression_start + 2])
              : NULL;
      if (!object || !object->agg || object->agg->size != 16 || !field ||
          field->offset != i * 8 || field->size != 8 || !expect(p, ")"))
        return defer_unsupported_asm(p, open, assembly);
      if (i == 0)
        old_object = object;
      else if (old_object != object)
        return defer_unsupported_asm(p, open, assembly);
      if (i == 0 && !expect(p, ","))
        return defer_unsupported_asm(p, open, assembly);
    }
    if (!expect(p, ":"))
      return defer_unsupported_asm(p, open, assembly);

    c_local_t *new_object = NULL;
    const char *input_constraints[2] = {"b", "c"};
    for (size_t i = 0; i < 2; ++i) {
      if ((i && !expect(p, ",")) || !parse_inline_asm_operand_name(p) ||
          !inline_asm_string_sequence_is(p, input_constraints[i], &p->pos) ||
          !expect(p, "("))
        return defer_unsupported_asm(p, open, assembly);
      size_t expression_start = p->pos;
      c_value_t field_value = parse_expr(p, 1);
      const c_lvalue_t *field_lvalue =
          field_value.value >= 0 ? find_lvalue(p, field_value.value) : NULL;
      if (!field_lvalue || field_lvalue->type != C_U64 ||
          expression_start + 2 >= p->token_count ||
          !tok_ident(p->tokens[expression_start]) ||
          !tok_is(p, expression_start + 1, "."))
        return defer_unsupported_asm(p, open, assembly);
      c_local_t *object = find_local(p, p->tokens[expression_start]);
      const c_agg_field_t *field =
          object && object->type == C_AGG
              ? find_field(object->agg, p->tokens[expression_start + 2])
              : NULL;
      if (!object || !object->agg || object->agg->size != 16 || !field ||
          field->offset != i * 8 || field->size != 8 || !expect(p, ")"))
        return defer_unsupported_asm(p, open, assembly);
      if (i == 0)
        new_object = object;
      else if (new_object != object)
        return defer_unsupported_asm(p, open, assembly);
    }
    if (!expect(p, ":") ||
        !inline_asm_string_sequence_is(p, "memory", &p->pos) ||
        !expect(p, ")") || !expect(p, ";"))
      return defer_unsupported_asm(p, open, assembly);
    c_local_t *condition =
        condition_name.len ? find_local(p, condition_name) : NULL;
    if ((condition_name.len &&
         (!condition || condition->type != C_U8 || condition->is_const ||
          condition->array_rank)) ||
        !old_object || !new_object || old_object == new_object)
      return defer_unsupported_asm(p, open, assembly);
    int old_address =
        emit_value(p, (nyr_inst_t){.op = NYIR_LOAD_LOCAL,
                                   .dst = -1,
                                   .imm = (int64_t)old_object->slot,
                                   .scalar_class = NYIR_SCALAR_I64_RAW});
    int new_address =
        emit_value(p, (nyr_inst_t){.op = NYIR_LOAD_LOCAL,
                                   .dst = -1,
                                   .imm = (int64_t)new_object->slot,
                                   .scalar_class = NYIR_SCALAR_I64_RAW});
    free(assembly);
    int result =
        old_address < 0 || new_address < 0
            ? -1
            : emit_value(p, (nyr_inst_t){.op = NYIR_INLINE_ASM,
                                         .dst = -1,
                                         .a = memory_address,
                                         .b = old_address,
                                         .c = new_address,
                                         .d = -1,
                                         .imm = -160,
                                         .symbol = locked ? "cmpxchg16b_try"
                                                          : "cmpxchg16b",
                                         .effects = NYIR_EFFECT_READ_MEMORY |
                                                    NYIR_EFFECT_WRITE_MEMORY |
                                                    NYIR_EFFECT_MAY_TRAP |
                                                    NYIR_EFFECT_VOLATILE});
    return result >= 0 &&
           (!condition || lower_local_store(p, condition, result));
  } else if (lower_classic_cmpxchg && accept(p, ":")) {
    size_t constraint_end = p->pos;
    ny_ctok_t output_name;
    if (!inline_asm_string_sequence_is(p, "=a", &constraint_end))
      return defer_unsupported_asm(p, open, assembly);
    p->pos = constraint_end;
    if (!expect(p, "(") || !parse_name(p, &output_name) || !expect(p, ")") ||
        !expect(p, ",") || !inline_asm_string_sequence_is(p, "+m", &p->pos) ||
        !expect(p, "("))
      return defer_unsupported_asm(p, open, assembly);
    c_value_t memory = parse_expr(p, 1);
    const c_lvalue_t *memory_lvalue =
        memory.value >= 0 ? find_lvalue(p, memory.value) : NULL;
    if (!memory_lvalue || memory_lvalue->address < 0 ||
        (!c_type_is_pointer(memory_lvalue->type) &&
         !c_type_is_integer(memory_lvalue->type) &&
         memory_lvalue->type != C_VA_LIST) ||
        !expect(p, ")") || !expect(p, ":") ||
        (!inline_asm_string_sequence_is(p, "r", &p->pos) &&
         !inline_asm_string_sequence_is(p, "q", &p->pos)) ||
        !expect(p, "("))
      return defer_unsupported_asm(p, open, assembly);
    c_type_t memory_type = memory_lvalue->type;
    int memory_address = memory_lvalue->address;
    c_value_t replacement = parse_expr(p, 1);
    if (replacement.value < 0 ||
        (!c_type_is_pointer(replacement.type) &&
         !c_type_is_integer(replacement.type) &&
         replacement.type != C_VA_LIST) ||
        !expect(p, ")") || !expect(p, ",") ||
        !inline_asm_string_sequence_is(p, "0", &p->pos) || !expect(p, "("))
      return defer_unsupported_asm(p, open, assembly);
    c_value_t expected = parse_expr(p, 1);
    c_local_t *output = find_local(p, output_name);
    if (expected.value < 0 ||
        (!c_type_is_pointer(expected.type) &&
         !c_type_is_integer(expected.type) && expected.type != C_VA_LIST) ||
        !expect(p, ")") || !expect(p, ":") ||
        !inline_asm_string_sequence_is(p, "memory", &p->pos) || !output ||
        output->array_rank || output->is_const ||
        !c_cmpxchg_type_compatible(p->abi, output->type, memory_type) ||
        !c_cmpxchg_type_compatible(p->abi, replacement.type, memory_type) ||
        !c_cmpxchg_type_compatible(p->abi, expected.type, memory_type))
      return defer_unsupported_asm(p, open, assembly);
    asm_output = output;
    asm_memory_output_value = memory_address;
    asm_cmpxchg_expected_value = expected.value;
    asm_cmpxchg_new_value = replacement.value;
    lower_cmpxchg_value_output = true;
    lower_cmpxchg64 = c_type_is_pointer(memory_type) || memory_type == C_I64 ||
                      memory_type == C_U64 || memory_type == C_VA_LIST;
    lower_cmpxchg8 = memory_type == C_CHAR || memory_type == C_U8;
    lower_cmpxchg16 = memory_type == C_I16 || memory_type == C_U16;
    free(assembly);
    assembly = (unsigned char *)strdup(
        lower_cmpxchg64   ? lower_classic_cmpxchg_locked
                                ? "lock cmpxchgq %rdx, (%rcx)"
                                : "cmpxchgq %rdx, (%rcx)"
        : lower_cmpxchg8  ? "lock cmpxchgb %dl, (%rcx)"
        : lower_cmpxchg16 ? "lock cmpxchgw %dx, (%rcx)"
                          : "lock cmpxchgl %edx, (%rcx)");
    if (!assembly) {
      fail(p, "out of memory lowering qword cmpxchg inline assembly");
      return false;
    }
    assembly_len = strlen((const char *)assembly);
    assembly_cap = assembly_len + 1;
  } else if (kexec_segment_load && accept(p, ":")) {
    if (!expect(p, ":") || !inline_asm_string_sequence_is(p, "a", &p->pos) ||
        !expect(p, "("))
      return defer_unsupported_asm(p, open, assembly);
    c_value_t value = parse_expr(p, 1);
    if (value.value < 0 || !c_type_is_integer(value.type) || !expect(p, ")") ||
        !expect(p, ":"))
      return defer_unsupported_asm(p, open, assembly);
    size_t clobber_end = p->pos;
    if (!inline_asm_clobber_list_is(p, &clobber_end) || clobber_end == p->pos)
      return defer_unsupported_asm(p, open, assembly);
    p->pos = clobber_end;
    const char *immediates[] = {"%eax"};
    if (!substitute_inline_asm_immediate(
            &assembly, &assembly_len, &assembly_cap, immediates, 1, SIZE_MAX))
      return defer_unsupported_asm(p, open, assembly);
    unescape_inline_asm_percents(assembly, &assembly_len);
    asm_segment_load_value = value.value;
    lower_segment_load_asm = true;
  } else if (apic_alt_io && accept(p, ":")) {
    ny_ctok_t output_name;
    if (!inline_asm_string_sequence_is(p, "=r", &p->pos) || !expect(p, "(") ||
        !parse_name(p, &output_name) || !expect(p, ")") || !expect(p, ",") ||
        !inline_asm_string_sequence_is(p, "=m", &p->pos) || !expect(p, "("))
      return defer_unsupported_asm(p, open, assembly);
    c_value_t output_memory = parse_expr(p, 1);
    const c_lvalue_t *output_lvalue =
        output_memory.value >= 0 ? find_lvalue(p, output_memory.value) : NULL;
    if (!output_lvalue || output_lvalue->address < 0 ||
        (output_lvalue->type != C_I32 && output_lvalue->type != C_U32) ||
        !expect(p, ")") || !expect(p, ":") ||
        !inline_asm_string_sequence_is(p, "i", &p->pos) || !expect(p, "("))
      return defer_unsupported_asm(p, open, assembly);
    c_value_t unused_alternative_index = parse_expr(p, 1);
    if (unused_alternative_index.value < 0 || !expect(p, ")") ||
        !expect(p, ",") || !inline_asm_string_sequence_is(p, "0", &p->pos) ||
        !expect(p, "("))
      return defer_unsupported_asm(p, open, assembly);
    c_value_t input = parse_expr(p, 1);
    if (input.value < 0 || !c_type_is_integer(input.type) || !expect(p, ")") ||
        !expect(p, ",") || !inline_asm_string_sequence_is(p, "m", &p->pos) ||
        !expect(p, "("))
      return defer_unsupported_asm(p, open, assembly);
    c_value_t input_memory = parse_expr(p, 1);
    const c_lvalue_t *input_lvalue =
        input_memory.value >= 0 ? find_lvalue(p, input_memory.value) : NULL;
    if (!input_lvalue || input_lvalue->address < 0 ||
        (input_lvalue->type != C_I32 && input_lvalue->type != C_U32) ||
        !expect(p, ")"))
      return defer_unsupported_asm(p, open, assembly);
    if (accept(p, ":")) {
      size_t clobber_end = p->pos;
      if (!inline_asm_clobber_list_is(p, &clobber_end))
        return defer_unsupported_asm(p, open, assembly);
      p->pos = clobber_end;
    }
    asm_alt_io_output = find_local(p, output_name);
    if (!asm_alt_io_output || asm_alt_io_output->is_const ||
        (asm_alt_io_output->type != C_I32 &&
         asm_alt_io_output->type != C_U32) ||
        !substitute_inline_asm_alt_io_operands(&assembly, &assembly_len,
                                               &assembly_cap))
      return defer_unsupported_asm(p, open, assembly);
    asm_alt_io_input = input.value;
    asm_alt_io_memory = output_lvalue->address;
    lower_apic_alt_io = true;
  } else if (!asm_goto && assembly_len == sizeof("jmp *%0") - 1 &&
             memcmp(assembly, "jmp *%0", assembly_len) == 0 && accept(p, ":")) {
    if (!expect(p, ":") || !inline_asm_string_sequence_is(p, "r", &p->pos) ||
        !expect(p, "("))
      return defer_unsupported_asm(p, open, assembly);
    c_value_t target = parse_expr(p, 1);
    if (target.value < 0 ||
        (!c_type_is_integer(target.type) && !c_type_is_pointer(target.type)) ||
        !expect(p, ")") || !expect(p, ",") ||
        !inline_asm_string_sequence_is(p, "S", &p->pos) || !expect(p, "("))
      return defer_unsupported_asm(p, open, assembly);
    c_value_t argument = parse_expr(p, 1);
    if (argument.value < 0 ||
        (!c_type_is_integer(argument.type) &&
         !c_type_is_pointer(argument.type)) ||
        !expect(p, ")"))
      return defer_unsupported_asm(p, open, assembly);
    if (accept(p, ":")) {
      size_t clobber_end = p->pos;
      if (!inline_asm_clobber_list_is(p, &clobber_end))
        return defer_unsupported_asm(p, open, assembly);
      p->pos = clobber_end;
    }
    asm_indirect_jump_target = target.value;
    asm_indirect_jump_argument = argument.value;
    lower_indirect_jump = true;
  } else if (assembly_len == sizeof("ljmpl *%0") - 1 &&
             memcmp(assembly, "ljmpl *%0", assembly_len) == 0 &&
             accept(p, ":")) {
    if (!expect(p, ":") || !inline_asm_string_sequence_is(p, "m", &p->pos) ||
        !expect(p, "("))
      return defer_unsupported_asm(p, open, assembly);
    c_value_t memory = parse_expr(p, 1);
    const c_lvalue_t *memory_lvalue =
        memory.value >= 0 ? find_lvalue(p, memory.value) : NULL;
    if (!memory_lvalue || memory_lvalue->address < 0 || !expect(p, ")") ||
        !expect(p, ",") || !inline_asm_string_sequence_is(p, "D", &p->pos) ||
        !expect(p, "("))
      return defer_unsupported_asm(p, open, assembly);
    c_value_t type = parse_expr(p, 1);
    if (type.value < 0 || !c_type_is_integer(type.type) || !expect(p, ")"))
      return defer_unsupported_asm(p, open, assembly);
    if (accept(p, ":")) {
      size_t clobber_end = p->pos;
      if (!inline_asm_clobber_list_is(p, &clobber_end))
        return defer_unsupported_asm(p, open, assembly);
      p->pos = clobber_end;
    }
    asm_far_jump_memory = memory_lvalue->address;
    asm_far_jump_type = type.value;
    lower_far_jump = true;
    free(assembly);
    assembly = (unsigned char *)strdup("ljmpl *(%rax)");
    if (!assembly) {
      fail(p, "out of memory lowering far indirect jump inline assembly");
      return false;
    }
    assembly_len = strlen((const char *)assembly);
    assembly_cap = assembly_len + 1;
  } else if (accept(p, ":")) {
    if (assembly_len == sizeof("mov %0, %%db0") - 1 &&
        memcmp(assembly, "mov %0, %%db", sizeof("mov %0, %%db") - 1) == 0 &&
        assembly[assembly_len - 1] >= '0' &&
        assembly[assembly_len - 1] <= '7') {
      if (!accept(p, ":"))
        return defer_unsupported_asm(p, open, assembly);
      size_t constraint_end = p->pos;
      if (!inline_asm_string_sequence_is(p, "r", &constraint_end))
        return defer_unsupported_asm(p, open, assembly);
      p->pos = constraint_end;
      if (!expect(p, "("))
        return defer_unsupported_asm(p, open, assembly);
      c_value_t input = parse_expr(p, 1);
      if (input.value < 0 || !c_type_is_integer(input.type) || !expect(p, ")"))
        return defer_unsupported_asm(p, open, assembly);
      if (accept(p, ":")) {
        size_t clobber_end = p->pos;
        if (!inline_asm_clobber_list_is(p, &clobber_end))
          return defer_unsupported_asm(p, open, assembly);
        p->pos = clobber_end;
      }
      asm_debug_input = input.value;
      asm_debug_reg = assembly[assembly_len - 1] - '0';
      free(assembly);
      assembly = NULL;
      goto finish_inline_asm_operands;
    }
    static const char runtime_const_ptr_prefix[] =
        "mov %1,%0\n1:\n.pushsection runtime_ptr_";
    static const char runtime_const_ptr_suffix[] =
        ",\"a\"\n\t.long 1b - %c2 - .\n.popsection";
    size_t runtime_const_ptr_prefix_len = sizeof(runtime_const_ptr_prefix) - 1;
    size_t runtime_const_ptr_suffix_len = sizeof(runtime_const_ptr_suffix) - 1;
    bool runtime_const_ptr_asm =
        assembly_len >
            runtime_const_ptr_prefix_len + runtime_const_ptr_suffix_len &&
        memcmp(assembly, runtime_const_ptr_prefix,
               runtime_const_ptr_prefix_len) == 0 &&
        memcmp(assembly + assembly_len - runtime_const_ptr_suffix_len,
               runtime_const_ptr_suffix, runtime_const_ptr_suffix_len) == 0;
    static const char runtime_const_shift_prefix[] =
        "shrl $12,%k0\n1:\n.pushsection runtime_shift_";
    size_t runtime_const_shift_prefix_len =
        sizeof(runtime_const_shift_prefix) - 1;
    const char *runtime_shift_prefix_end =
        assembly_len > runtime_const_shift_prefix_len &&
                memcmp(assembly, runtime_const_shift_prefix,
                       runtime_const_shift_prefix_len) == 0
            ? (const char *)assembly + runtime_const_shift_prefix_len
            : NULL;
    const char *runtime_shift_attribute =
        runtime_shift_prefix_end ? strstr(runtime_shift_prefix_end, ",\"a\"")
                                 : NULL;
    const char *runtime_shift_long =
        runtime_shift_attribute
            ? strstr(runtime_shift_attribute, ".long 1b - 1 - .")
            : NULL;
    const char *runtime_shift_pop =
        runtime_shift_long ? strstr(runtime_shift_long, ".popsection") : NULL;
    bool runtime_const_shift_asm = runtime_shift_pop != NULL;
    if (runtime_const_shift_asm) {
      size_t section_len =
          (size_t)(runtime_shift_attribute - runtime_shift_prefix_end);
      if (!section_len || section_len >= sizeof(runtime_shift_section))
        return defer_unsupported_asm(p, open, assembly);
      for (size_t i = 0; i < section_len; ++i) {
        unsigned char ch = assembly[runtime_const_shift_prefix_len + i];
        bool valid = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
                     (ch >= '0' && ch <= '9') || ch == '_';
        if (!valid || (!i && ch >= '0' && ch <= '9'))
          return defer_unsupported_asm(p, open, assembly);
        runtime_shift_section[i] = (char)ch;
      }
      size_t constraint_end = p->pos;
      ny_ctok_t output_name;
      if (!inline_asm_string_sequence_is(p, "+r", &constraint_end))
        return defer_unsupported_asm(p, open, assembly);
      p->pos = constraint_end;
      if (!expect(p, "(") || !parse_name(p, &output_name) || !expect(p, ")"))
        return defer_unsupported_asm(p, open, assembly);
      asm_output = find_local(p, output_name);
      if (!asm_output || asm_output->array_rank || asm_output->is_const ||
          !c_type_is_integer(asm_output->type))
        return defer_unsupported_asm(p, open, assembly);
      lower_runtime_const_shift = true;
      goto finish_inline_asm_operands;
    }
    if (runtime_const_ptr_asm) {
      size_t section_symbol_len = assembly_len - runtime_const_ptr_prefix_len -
                                  runtime_const_ptr_suffix_len;
      unsigned char first = assembly[runtime_const_ptr_prefix_len];
      runtime_const_ptr_asm = (first >= 'A' && first <= 'Z') ||
                              (first >= 'a' && first <= 'z') || first == '_';
      for (size_t i = 1; runtime_const_ptr_asm && i < section_symbol_len; ++i) {
        unsigned char ch = assembly[runtime_const_ptr_prefix_len + i];
        runtime_const_ptr_asm = (ch >= 'A' && ch <= 'Z') ||
                                (ch >= 'a' && ch <= 'z') ||
                                (ch >= '0' && ch <= '9') || ch == '_';
      }
    }
    if (runtime_const_ptr_asm) {
      size_t constraint_end = p->pos;
      ny_ctok_t output_name;
      if (!inline_asm_string_sequence_is(p, "=r", &constraint_end))
        return defer_unsupported_asm(p, open, assembly);
      p->pos = constraint_end;
      if (!expect(p, "(") || !parse_name(p, &output_name) || !expect(p, ")") ||
          !expect(p, ":"))
        return defer_unsupported_asm(p, open, assembly);
      asm_output = find_local(p, output_name);
      if (!asm_output || asm_output->array_rank || asm_output->is_const ||
          (asm_output->type != C_U64 && asm_output->type != C_I64 &&
           (p->abi != NYC_ABI_SYSV_X86_64 ||
            !c_type_is_pointer(asm_output->type))))
        return defer_unsupported_asm(p, open, assembly);

      int64_t immediate = 0;
      for (size_t operand = 0; operand < 2; ++operand) {
        if (operand && !expect(p, ","))
          return defer_unsupported_asm(p, open, assembly);
        constraint_end = p->pos;
        if (!inline_asm_string_sequence_is(p, "i", &constraint_end))
          return defer_unsupported_asm(p, open, assembly);
        p->pos = constraint_end;
        if (!expect(p, "("))
          return defer_unsupported_asm(p, open, assembly);
        c_value_t value = parse_expr(p, 1);
        int64_t constant = 0;
        if (value.value < 0 || !c_type_is_integer(value.type) ||
            !expect(p, ")") ||
            !c_ir_nonnegative_constant(p, value.value, &constant) ||
            (operand == 0 && constant != INT64_C(0x0123456789abcdef)) ||
            (operand == 1 && constant != (int64_t)sizeof(long)))
          return defer_unsupported_asm(p, open, assembly);
        if (operand == 0)
          immediate = constant;
        else
          snprintf(immediate_storage[2], sizeof(immediate_storage[2]), "%lld",
                   (long long)constant);
      }
      if (accept(p, ":")) {
        size_t clobber_end = p->pos;
        if (!inline_asm_clobber_list_is(p, &clobber_end))
          return defer_unsupported_asm(p, open, assembly);
        p->pos = clobber_end;
      }
      snprintf(immediate_storage[1], sizeof(immediate_storage[1]), "$%lld",
               (long long)immediate);
      immediate_values[0] = "%rax";
      immediate_values[1] = immediate_storage[1];
      immediate_values[2] = immediate_storage[2];
      if (!substitute_inline_asm_immediate(&assembly, &assembly_len,
                                           &assembly_cap, immediate_values, 3,
                                           SIZE_MAX))
        return defer_unsupported_asm(p, open, assembly);
      lower_runtime_const_ptr = true;
    } else if (assembly_len == sizeof("cmp %1,%0\n\tcmova %1,%0") - 1 &&
               memcmp(assembly, "cmp %1,%0\n\tcmova %1,%0", assembly_len) ==
                   0) {
      size_t output_constraint_end = p->pos;
      ny_ctok_t output_name;
      if (!inline_asm_string_sequence_is(p, "=r", &output_constraint_end))
        return defer_unsupported_asm(p, open, assembly);
      p->pos = output_constraint_end;
      if (!expect(p, "(") || !parse_name(p, &output_name) || !expect(p, ")") ||
          !expect(p, ":"))
        return defer_unsupported_asm(p, open, assembly);
      size_t input_constraint_end = p->pos;
      if (!inline_asm_string_sequence_is(p, "r", &input_constraint_end))
        return defer_unsupported_asm(p, open, assembly);
      p->pos = input_constraint_end;
      if (!expect(p, "("))
        return defer_unsupported_asm(p, open, assembly);
      c_value_t bound = parse_expr(p, 1);
      if (bound.value < 0 || !c_type_is_integer(bound.type) ||
          !expect(p, ")") || !expect(p, ","))
        return defer_unsupported_asm(p, open, assembly);
      input_constraint_end = p->pos;
      if (!inline_asm_string_sequence_is(p, "0", &input_constraint_end))
        return defer_unsupported_asm(p, open, assembly);
      p->pos = input_constraint_end;
      if (!expect(p, "("))
        return defer_unsupported_asm(p, open, assembly);
      c_value_t initial = parse_expr(p, 1);
      if (initial.value < 0 ||
          !(c_type_is_integer(initial.type) ||
            c_type_is_pointer(initial.type)) ||
          !expect(p, ")"))
        return defer_unsupported_asm(p, open, assembly);
      asm_output = find_local(p, output_name);
      if (!asm_output || asm_output->array_rank || asm_output->is_const ||
          !(c_type_is_integer(asm_output->type) ||
            c_type_is_pointer(asm_output->type)))
        return defer_unsupported_asm(p, open, assembly);
      if (accept(p, ":")) {
        size_t clobber_end = p->pos;
        if (!inline_asm_clobber_list_is(p, &clobber_end))
          return defer_unsupported_asm(p, open, assembly);
        p->pos = clobber_end;
      }
      runtime_ptr_clamp_bound = bound.value;
      runtime_ptr_clamp_initial = initial.value;
      lower_runtime_ptr_clamp = true;
    } else if (assembly_len == sizeof("tzcnt %1,%0") - 1 &&
               memcmp(assembly, "tzcnt %1,%0", assembly_len) == 0) {
      size_t output_constraint_end = p->pos;
      if (!inline_asm_string_sequence_is(p, "=r", &output_constraint_end))
        return defer_unsupported_asm(p, open, assembly);
      p->pos = output_constraint_end;
      ny_ctok_t output_name;
      if (!expect(p, "(") || !parse_name(p, &output_name) || !expect(p, ")") ||
          !expect(p, ":"))
        return defer_unsupported_asm(p, open, assembly);
      size_t input_constraint_end = p->pos;
      if (!inline_asm_string_sequence_is(p, "rm", &input_constraint_end))
        return defer_unsupported_asm(p, open, assembly);
      p->pos = input_constraint_end;
      if (!expect(p, "("))
        return defer_unsupported_asm(p, open, assembly);
      c_value_t input = parse_expr(p, 1);
      if (input.value < 0 || !expect(p, ")"))
        return defer_unsupported_asm(p, open, assembly);
      asm_output = find_local(p, output_name);
      if (!asm_output || asm_output->array_rank || asm_output->is_const ||
          (asm_output->type != C_U64 && asm_output->type != C_I64) ||
          (input.type != C_U64 && input.type != C_I64))
        return defer_unsupported_asm(p, open, assembly);
      if (accept(p, ":")) {
        size_t clobber_end = p->pos;
        if (!inline_asm_clobber_list_is(p, &clobber_end))
          return defer_unsupported_asm(p, open, assembly);
        p->pos = clobber_end;
      }
      lower_tzcnt = true;
      asm_tzcnt_input = input.value;
    } else if (assembly_len == sizeof("bsrl %1,%0") - 1 &&
               memcmp(assembly, "bsrl %1,%0", assembly_len) == 0) {
      size_t output_constraint_end = p->pos;
      if (!inline_asm_string_sequence_is(p, "=r", &output_constraint_end))
        return defer_unsupported_asm(p, open, assembly);
      p->pos = output_constraint_end;
      ny_ctok_t output_name;
      if (!expect(p, "(") || !parse_name(p, &output_name) || !expect(p, ")") ||
          !expect(p, ":"))
        return defer_unsupported_asm(p, open, assembly);
      size_t input_constraint_end = p->pos;
      if (!inline_asm_string_sequence_is(p, "rm", &input_constraint_end))
        return defer_unsupported_asm(p, open, assembly);
      p->pos = input_constraint_end;
      if (!expect(p, "("))
        return defer_unsupported_asm(p, open, assembly);
      c_value_t input = parse_expr(p, 1);
      if (input.value < 0 || !c_type_is_integer(input.type) ||
          !expect(p, ")") || !expect(p, ",") ||
          !inline_asm_string_sequence_is(p, "0", &p->pos) || !expect(p, "("))
        return defer_unsupported_asm(p, open, assembly);
      int64_t initial_value = 0;
      if (!parse_global_const_expr(p, &p->pos, &initial_value) ||
          initial_value != -1 || !expect(p, ")"))
        return defer_unsupported_asm(p, open, assembly);
      asm_output = find_local(p, output_name);
      if (!asm_output || asm_output->array_rank || asm_output->is_const ||
          (asm_output->type != C_I32 && asm_output->type != C_U32))
        return defer_unsupported_asm(p, open, assembly);
      if (accept(p, ":")) {
        size_t clobber_end = p->pos;
        if (!inline_asm_clobber_list_is(p, &clobber_end))
          return defer_unsupported_asm(p, open, assembly);
        p->pos = clobber_end;
      }
      int initial = emit_value(
          p, (nyr_inst_t){.op = NYIR_CONST_I64, .dst = -1, .imm = -1});
      if (initial < 0 || !lower_local_store(p, asm_output, initial))
        return defer_unsupported_asm(p, open, assembly);
      asm_bsr_input = normalize_integer(p, C_U32, input.value);
      if (asm_bsr_input < 0)
        return defer_unsupported_asm(p, open, assembly);
      lower_bsr = true;
    } else if (assembly_len == sizeof("bsrq %1,%q0") - 1 &&
               memcmp(assembly, "bsrq %1,%q0", assembly_len) == 0) {
      size_t output_constraint_end = p->pos;
      if (!inline_asm_string_sequence_is(p, "+r", &output_constraint_end))
        return defer_unsupported_asm(p, open, assembly);
      p->pos = output_constraint_end;
      ny_ctok_t output_name;
      if (!expect(p, "(") || !parse_name(p, &output_name) || !expect(p, ")") ||
          !expect(p, ":"))
        return defer_unsupported_asm(p, open, assembly);
      size_t input_constraint_end = p->pos;
      if (!inline_asm_string_sequence_is(p, "rm", &input_constraint_end))
        return defer_unsupported_asm(p, open, assembly);
      p->pos = input_constraint_end;
      if (!expect(p, "("))
        return defer_unsupported_asm(p, open, assembly);
      c_value_t input = parse_expr(p, 1);
      if (input.value < 0 || !c_type_is_integer(input.type) || !expect(p, ")"))
        return defer_unsupported_asm(p, open, assembly);
      asm_output = find_local(p, output_name);
      if (!asm_output || asm_output->array_rank || asm_output->is_const ||
          !c_type_is_integer(asm_output->type))
        return defer_unsupported_asm(p, open, assembly);
      if (accept(p, ":")) {
        size_t clobber_end = p->pos;
        if (!inline_asm_clobber_list_is(p, &clobber_end))
          return defer_unsupported_asm(p, open, assembly);
        p->pos = clobber_end;
      }
      lower_bsr = true;
      asm_bsr_input = input.value;
    } else if (assembly_len == sizeof("bsr %1,%0") - 1 &&
               memcmp(assembly, "bsr %1,%0", assembly_len) == 0) {
      size_t output_constraint_end = p->pos;
      if (!inline_asm_string_sequence_is(p, "=r", &output_constraint_end))
        return defer_unsupported_asm(p, open, assembly);
      p->pos = output_constraint_end;
      ny_ctok_t output_name;
      if (!expect(p, "(") || !parse_name(p, &output_name) || !expect(p, ")") ||
          !expect(p, ":") || !inline_asm_string_sequence_is(p, "rm", &p->pos) ||
          !expect(p, "("))
        return defer_unsupported_asm(p, open, assembly);
      c_value_t input = parse_expr(p, 1);
      if (input.value < 0 || (input.type != C_I64 && input.type != C_U64) ||
          !expect(p, ")"))
        return defer_unsupported_asm(p, open, assembly);
      asm_output = find_local(p, output_name);
      if (!asm_output || asm_output->array_rank || asm_output->is_const ||
          (asm_output->type != C_I64 && asm_output->type != C_U64))
        return defer_unsupported_asm(p, open, assembly);
      if (accept(p, ":")) {
        size_t clobber_end = p->pos;
        if (!inline_asm_clobber_list_is(p, &clobber_end))
          return defer_unsupported_asm(p, open, assembly);
        p->pos = clobber_end;
      }
      int initial = emit_value(
          p, (nyr_inst_t){.op = NYIR_CONST_I64, .dst = -1, .imm = 0});
      if (initial < 0 || !lower_local_store(p, asm_output, initial))
        return defer_unsupported_asm(p, open, assembly);
      lower_bsr = true;
      asm_bsr_input = input.value;
    } else if (assembly_len == sizeof("invpcid %[desc], %[type]") - 1 &&
               memcmp(assembly, "invpcid %[desc], %[type]", assembly_len) ==
                   0) {
      if (!expect(p, ":"))
        return defer_unsupported_asm(p, open, assembly);
      ny_ctok_t desc_name, type_name;
      if (!parse_named_inline_asm_operand(p, &desc_name) ||
          !inline_asm_string_sequence_is(p, "m", &p->pos) || !expect(p, "("))
        return defer_unsupported_asm(p, open, assembly);
      c_value_t desc = parse_expr(p, 1);
      const c_lvalue_t *desc_lvalue =
          desc.value >= 0 ? find_lvalue(p, desc.value) : NULL;
      if (!desc_lvalue || desc_lvalue->address < 0 || !expect(p, ")") ||
          !expect(p, ",") || !parse_named_inline_asm_operand(p, &type_name) ||
          !inline_asm_string_sequence_is(p, "r", &p->pos) || !expect(p, "("))
        return defer_unsupported_asm(p, open, assembly);
      c_value_t type = parse_expr(p, 1);
      (void)desc_name;
      (void)type_name;
      if (type.value < 0 || !c_type_is_integer(type.type) || !expect(p, ")"))
        return defer_unsupported_asm(p, open, assembly);
      asm_memory_input_value = desc_lvalue->address;
      asm_memory_register_input_value = type.value;
      if (accept(p, ":")) {
        size_t clobber_end = p->pos;
        if (!inline_asm_clobber_list_is(p, &clobber_end))
          return defer_unsupported_asm(p, open, assembly);
        p->pos = clobber_end;
      }
      lower_invpcid = true;
    } else if (accept(p, ":")) {
      size_t empty_barrier_constraint_end = p->pos;
      if (!assembly_len && inline_asm_string_sequence_is(
                               p, "r", &empty_barrier_constraint_end)) {
        p->pos = empty_barrier_constraint_end;
        if (!expect(p, "("))
          return defer_unsupported_asm(p, open, assembly);
        c_value_t input = parse_expr(p, 1);
        if (input.value < 0 ||
            (!c_type_is_integer(input.type) &&
             !c_type_is_pointer(input.type)) ||
            !expect(p, ")") || !expect(p, ":"))
          return defer_unsupported_asm(p, open, assembly);
        size_t clobber_end = p->pos;
        if (!inline_asm_clobber_list_is(p, &clobber_end))
          return defer_unsupported_asm(p, open, assembly);
        p->pos = clobber_end;
        lower_empty_memory_barrier = true;
      } else if (!assembly_len &&
                 inline_asm_string_sequence_is(p, "+rm",
                                               &empty_barrier_constraint_end)) {
        p->pos = empty_barrier_constraint_end;
        if (!expect(p, "("))
          return defer_unsupported_asm(p, open, assembly);
        c_value_t output = parse_expr(p, 1);
        const c_lvalue_t *recorded =
            output.value >= 0 ? find_lvalue(p, output.value) : NULL;
        if (!recorded || !expect(p, ")") || !discard_lvalue_read(p, recorded))
          return defer_unsupported_asm(p, open, assembly);
        lower_empty_memory_barrier = true;
      } else if (tok_is(p, p->pos, ":") &&
                 !(p->tokens[p->pos + 1].kind == NY_CTOK_STRING &&
                   tok_is(p, p->pos + 2, "("))) {
        ++p->pos;
        size_t clobber_end = p->pos;
        if (!inline_asm_clobber_list_is(p, &clobber_end)) {
          fail(p, "unsupported GNU asm clobber list");
          free(assembly);
          return false;
        }
        p->pos = clobber_end;
      } else if (!tok_is(p, p->pos, ")")) {
        for (;;) {
          if (immediate_count ==
              sizeof(immediate_values) / sizeof(immediate_values[0]))
            return defer_unsupported_asm(p, open, assembly);
          size_t standalone_memory_constraint_end = p->pos;
          c_parser_t memory_probe = *p;
          bool is_memory_input =
              parse_inline_asm_operand_name(&memory_probe) &&
              inline_asm_string_sequence_is(&memory_probe, "m",
                                            &standalone_memory_constraint_end);
          if (is_memory_input && immediate_count == 0) {
            p->pos = standalone_memory_constraint_end;
            if (!expect(p, "("))
              return defer_unsupported_asm(p, open, assembly);
            c_value_t memory_input = parse_expr(p, 1);
            if (memory_input.value < 0 || !expect(p, ")"))
              return defer_unsupported_asm(p, open, assembly);
            const c_lvalue_t *memory_lvalue =
                find_asm_memory_lvalue(p, memory_input.value);
            int memory_address = memory_lvalue && memory_lvalue->address >= 0
                                     ? memory_lvalue->address
                                 : memory_input.type == C_AGG
                                     ? memory_input.value
                                     : -1;
            if (memory_address < 0)
              return defer_unsupported_asm(p, open, assembly);
            asm_memory_input_value = memory_address;
            immediate_values[0] = NULL;
            immediate_count = 1;
            memory_operand_index = 0;
            if (accept(p, ",")) {
              size_t register_constraint_end = p->pos;
              if (!parse_inline_asm_operand_name(p) ||
                  !inline_asm_string_sequence_is(p, "Ir",
                                                 &register_constraint_end))
                return defer_unsupported_asm(p, open, assembly);
              p->pos = register_constraint_end;
              if (!expect(p, "("))
                return defer_unsupported_asm(p, open, assembly);
              c_value_t register_input = parse_expr(p, 1);
              if (register_input.value < 0 ||
                  !c_type_is_integer(register_input.type) || !expect(p, ")"))
                return defer_unsupported_asm(p, open, assembly);
              asm_memory_register_input_value = register_input.value;
              int written = snprintf(immediate_storage[1],
                                     sizeof(immediate_storage[1]), "%%%%rdx");
              if (written < 0 ||
                  (size_t)written >= sizeof(immediate_storage[1]))
                return defer_unsupported_asm(p, open, assembly);
              immediate_values[1] = immediate_storage[1];
              immediate_count = 2;
            }
            break;
          }
          size_t register_constraint_end = p->pos;
          c_parser_t register_probe = *p;
          int register_index =
              parse_inline_asm_operand_name(&register_probe) &&
                      inline_asm_string_sequence_is(&register_probe, "a",
                                                    &register_constraint_end)
                  ? 0
              : inline_asm_string_sequence_is(&register_probe, "c",
                                              &register_constraint_end)
                  ? 1
              : inline_asm_string_sequence_is(&register_probe, "d",
                                              &register_constraint_end) ||
                      inline_asm_string_sequence_is(&register_probe, "Nd",
                                                    &register_constraint_end)
                  ? 2
                  : -1;
          if (register_index >= 0) {
            if (immediate_count ||
                asm_fixed_register_inputs[register_index] >= 0)
              return defer_unsupported_asm(p, open, assembly);
            p->pos = register_constraint_end;
            if (!expect(p, "("))
              return defer_unsupported_asm(p, open, assembly);
            c_value_t input = parse_expr(p, 1);
            if (input.value < 0 || !expect(p, ")") ||
                !c_type_is_integer(input.type))
              return defer_unsupported_asm(p, open, assembly);
            asm_fixed_register_inputs[register_index] = input.value;
            lower_fixed_register_input = true;
            if (!accept(p, ","))
              break;
            continue;
          }
          size_t input_constraint_end = p->pos;
          ny_ctok_t input_name = {0};
          if (!parse_named_inline_asm_operand(p, &input_name) ||
              !inline_asm_string_sequence_is(p, "i", &input_constraint_end))
            return defer_unsupported_asm(p, open, assembly);
          p->pos = input_constraint_end;
          if (!expect(p, "(")) {
            free(assembly);
            return false;
          }
          bool string_immediate = current(p)->kind == NY_CTOK_STRING;
          if (string_immediate) {
            size_t global_count = p->global_count;
            c_value_t string_value = lower_string_literal(p);
            if (string_value.value < 0 || p->global_count != global_count + 1)
              return defer_unsupported_asm(p, open, assembly);
            ny_ctok_t string_name = p->globals[global_count].name;
            int written = snprintf(immediate_storage[immediate_count],
                                   sizeof(immediate_storage[immediate_count]),
                                   "__nyc_data_%.*s", (int)string_name.len,
                                   string_name.start);
            if (written < 0 ||
                (size_t)written >= sizeof(immediate_storage[immediate_count]))
              return defer_unsupported_asm(p, open, assembly);
            if (!expect(p, ")")) {
              free(assembly);
              return false;
            }
          } else {
            size_t value_pos = p->pos;
            int64_t value = 0;
            bool previous_asm_constant_context = p->inline_asm_constant_context;
            p->inline_asm_constant_context = true;
            bool constant = parse_global_const_expr(p, &p->pos, &value);
            p->inline_asm_constant_context = previous_asm_constant_context;
            if (!constant) {
              p->pos = value_pos;
              if (current(p)->kind == NY_CTOK_IDENT) {
                c_local_t *constant_local = find_local(p, *current(p));
                if (constant_local && constant_local->has_integer_constant &&
                    !constant_local->address_taken &&
                    !constant_local->is_volatile &&
                    !constant_local->static_name.len) {
                  value = constant_local->integer_constant;
                  ++p->pos;
                  constant = true;
                }
              }
            }
            if (!constant || !expect(p, ")")) {
              p->pos = value_pos;
              return defer_unsupported_asm(p, open, assembly);
            }
            int written = snprintf(immediate_storage[immediate_count],
                                   sizeof(immediate_storage[immediate_count]),
                                   "%lld", (long long)value);
            if (written < 0 ||
                (size_t)written >= sizeof(immediate_storage[immediate_count]))
              return defer_unsupported_asm(p, open, assembly);
          }
          immediate_values[immediate_count] =
              immediate_storage[immediate_count];
          if (input_name.kind == NY_CTOK_IDENT) {
            if (asm_named_input_count ==
                sizeof(asm_named_inputs) / sizeof(asm_named_inputs[0]))
              return defer_unsupported_asm(p, open, assembly);
            asm_named_inputs[asm_named_input_count++] =
                (inline_asm_named_value_t){
                    .name = input_name,
                    .replacement = immediate_storage[immediate_count]};
          }
          ++immediate_count;
          if (!accept(p, ","))
            break;
          c_parser_t next_input = *p;
          size_t next_constraint_end = next_input.pos;
          bool is_next_immediate = parse_inline_asm_operand_name(&next_input) &&
                                   inline_asm_string_sequence_is(
                                       &next_input, "i", &next_constraint_end);
          if (is_next_immediate)
            continue;
          size_t memory_constraint_end = p->pos;
          if (!parse_inline_asm_operand_name(p) ||
              !inline_asm_string_sequence_is(p, "m", &memory_constraint_end) ||
              immediate_count != 1)
            return defer_unsupported_asm(p, open, assembly);
          p->pos = memory_constraint_end;
          if (!expect(p, "("))
            return defer_unsupported_asm(p, open, assembly);
          c_value_t memory_input = parse_expr(p, 1);
          if (memory_input.value < 0 || !expect(p, ")")) {
            free(assembly);
            return false;
          }
          const c_lvalue_t *memory_lvalue =
              find_asm_memory_lvalue(p, memory_input.value);
          if (!memory_lvalue || memory_lvalue->address < 0)
            return defer_unsupported_asm(p, open, assembly);
          asm_memory_input_value = memory_lvalue->address;
          memory_operand_index = immediate_count;
          break;
        }
        bool has_immediate_placeholder = false;
        for (size_t i = 0; i + 1 < assembly_len; ++i) {
          if (inline_asm_immediate_placeholder(assembly, assembly_len, i,
                                               NULL)) {
            has_immediate_placeholder = true;
            break;
          }
        }
        if ((immediate_count || memory_operand_index != SIZE_MAX) &&
            has_immediate_placeholder &&
            !substitute_inline_asm_immediate(
                &assembly, &assembly_len, &assembly_cap, immediate_values,
                immediate_count, memory_operand_index)) {
          fail(p, "unsupported inline-asm immediate template");
          free(assembly);
          return false;
        }
        if (asm_named_input_count &&
            !substitute_inline_asm_named_operands(
                &assembly, &assembly_len, &assembly_cap, (ny_ctok_t){0}, NULL,
                asm_named_inputs, asm_named_input_count))
          return defer_unsupported_asm(p, open, assembly);
        if (accept(p, ":")) {
          size_t clobber_end = p->pos;
          if (!inline_asm_clobber_list_is(p, &clobber_end))
            return defer_unsupported_asm(p, open, assembly);
          p->pos = clobber_end;
        }
      }
    } else {
      if (lower_classic_cmpxchg && !expect(p, ":"))
        return defer_unsupported_asm(p, open, assembly);
      /*
       * Lower Linux x86-64 cmpxchg idioms using their fixed accumulator,
       * memory, replacement-register, and optional ZF output constraints.
       */
      size_t condition_constraint_end = p->pos;
      if (inline_asm_string_sequence_is(p, "=a", &condition_constraint_end)) {
        p->pos = condition_constraint_end;
        ny_ctok_t output_name;
        c_type_t memory_type = C_BAD;
        int memory_address = -1;
        if (!expect(p, "(") || !parse_name(p, &output_name) ||
            !expect(p, ")") || !expect(p, ",") ||
            !inline_asm_string_sequence_is(p, "+m", &p->pos) || !expect(p, "("))
          return defer_unsupported_asm(p, open, assembly);
        c_value_t memory = parse_expr(p, 1);
        const c_lvalue_t *memory_lvalue =
            memory.value >= 0 ? find_lvalue(p, memory.value) : NULL;
        if (!memory_lvalue || memory_lvalue->address < 0 ||
            (!c_type_is_pointer(memory_lvalue->type) &&
             memory_lvalue->type != C_I64 && memory_lvalue->type != C_U64) ||
            !expect(p, ")") || !expect(p, ":") ||
            !inline_asm_string_sequence_is(p, "r", &p->pos) || !expect(p, "("))
          return defer_unsupported_asm(p, open, assembly);
        memory_type = memory_lvalue->type;
        memory_address = memory_lvalue->address;
        c_value_t replacement = parse_expr(p, 1);
        if (replacement.value < 0 ||
            (!c_type_is_pointer(replacement.type) &&
             replacement.type != C_I64 && replacement.type != C_U64) ||
            !expect(p, ")") || !expect(p, ",") ||
            !inline_asm_string_sequence_is(p, "0", &p->pos) || !expect(p, "("))
          return defer_unsupported_asm(p, open, assembly);
        c_value_t expected = parse_expr(p, 1);
        asm_output = find_local(p, output_name);
        if (expected.value < 0 ||
            (!c_type_is_pointer(expected.type) && expected.type != C_I64 &&
             expected.type != C_U64) ||
            !expect(p, ")") || !expect(p, ":") ||
            !inline_asm_string_sequence_is(p, "memory", &p->pos) ||
            !expect(p, ")") || !expect(p, ";") || !asm_output ||
            asm_output->array_rank || asm_output->is_const ||
            (!c_type_is_pointer(asm_output->type) &&
             asm_output->type != C_I64 && asm_output->type != C_U64) ||
            asm_output->type != memory_type ||
            replacement.type != memory_type || expected.type != memory_type)
          return defer_unsupported_asm(p, open, assembly);
        asm_memory_output_value = memory_address;
        asm_cmpxchg_expected_value = expected.value;
        asm_cmpxchg_new_value = replacement.value;
        lower_cmpxchg_value_output = true;
        lower_cmpxchg64 = true;
        free(assembly);
        assembly = (unsigned char *)strdup(lower_classic_cmpxchg_locked
                                               ? "lock cmpxchgq %rdx, (%rcx)"
                                               : "cmpxchgq %rdx, (%rcx)");
        if (!assembly) {
          fail(p, "out of memory lowering qword cmpxchg inline assembly");
          return false;
        }
        assembly_len = strlen((const char *)assembly);
        assembly_cap = assembly_len + 1;
      } else if (inline_asm_string_sequence_is(p, "=@ccz",
                                               &condition_constraint_end)) {
        p->pos = condition_constraint_end;
        ny_ctok_t condition_name;
        ny_ctok_t old_name;
        ny_ctok_t memory_name;
        ny_ctok_t new_name;
        ny_ctok_t old_local_name;
        c_type_t memory_type;
        unsigned memory_segment;
        int memory_address;
        if (!expect(p, "(") || !parse_name(p, &condition_name) ||
            !expect(p, ")") || !expect(p, ",") ||
            !parse_named_inline_asm_operand(p, &memory_name))
          return defer_unsupported_asm(p, open, assembly);
        size_t constraint_end = p->pos;
        bool memory_first =
            memory_name.kind == NY_CTOK_IDENT &&
            inline_asm_string_sequence_is(p, "+m", &constraint_end);
        if (!memory_first) {
          old_name = memory_name;
          constraint_end = p->pos;
          if (old_name.kind != NY_CTOK_IDENT ||
              !inline_asm_string_sequence_is(p, "+a", &constraint_end))
            return defer_unsupported_asm(p, open, assembly);
          p->pos = constraint_end;
          if (!expect(p, "(") || !parse_name(p, &old_local_name) ||
              !expect(p, ")") || !expect(p, ",") ||
              !parse_named_inline_asm_operand(p, &memory_name))
            return defer_unsupported_asm(p, open, assembly);
          constraint_end = p->pos;
          if (memory_name.kind != NY_CTOK_IDENT ||
              !inline_asm_string_sequence_is(p, "+m", &constraint_end))
            return defer_unsupported_asm(p, open, assembly);
        }
        if (memory_name.kind != NY_CTOK_IDENT)
          return defer_unsupported_asm(p, open, assembly);
        p->pos = constraint_end;
        if (!expect(p, "("))
          return defer_unsupported_asm(p, open, assembly);
        c_value_t memory = parse_expr(p, 1);
        const c_lvalue_t *memory_lvalue =
            memory.value >= 0 ? find_lvalue(p, memory.value) : NULL;
        if (!memory_lvalue ||
            (!c_type_is_pointer(memory_lvalue->type) &&
             memory_lvalue->type != C_U8 && memory_lvalue->type != C_CHAR &&
             memory_lvalue->type != C_I16 && memory_lvalue->type != C_U16 &&
             memory_lvalue->type != C_I32 && memory_lvalue->type != C_U32 &&
             memory_lvalue->type != C_I64 && memory_lvalue->type != C_U64) ||
            memory_lvalue->address < 0 || !expect(p, ")"))
          return defer_unsupported_asm(p, open, assembly);
        memory_type = memory_lvalue->type;
        memory_segment = memory_lvalue->segment_flags;
        memory_address = memory_lvalue->address;
        if (!discard_lvalue_read(p, memory_lvalue))
          return defer_unsupported_asm(p, open, assembly);
        if (memory_first) {
          if (!expect(p, ",") || !parse_named_inline_asm_operand(p, &old_name))
            return defer_unsupported_asm(p, open, assembly);
          constraint_end = p->pos;
          if (old_name.kind != NY_CTOK_IDENT ||
              !inline_asm_string_sequence_is(p, "+a", &constraint_end))
            return defer_unsupported_asm(p, open, assembly);
          p->pos = constraint_end;
          if (!expect(p, "(") || !parse_name(p, &old_local_name) ||
              !expect(p, ")"))
            return defer_unsupported_asm(p, open, assembly);
        }
        if (!expect(p, ":") || !parse_named_inline_asm_operand(p, &new_name))
          return defer_unsupported_asm(p, open, assembly);
        constraint_end = p->pos;
        bool new_reg_constraint =
            inline_asm_string_sequence_is(p, "r", &constraint_end) ||
            inline_asm_string_sequence_is(p, "q", &constraint_end);
        if (new_name.kind != NY_CTOK_IDENT || !new_reg_constraint)
          return defer_unsupported_asm(p, open, assembly);
        p->pos = constraint_end;
        if (!expect(p, "("))
          return defer_unsupported_asm(p, open, assembly);
        c_value_t new_value = parse_expr(p, 1);
        if (new_value.value < 0 || !expect(p, ")") || !expect(p, ":"))
          return defer_unsupported_asm(p, open, assembly);
        size_t clobber_end = p->pos;
        if (!inline_asm_clobber_list_is(p, &clobber_end))
          return defer_unsupported_asm(p, open, assembly);
        p->pos = clobber_end;
        asm_condition_output = find_local(p, condition_name);
        asm_output = find_local(p, old_local_name);
        lower_cmpxchg64 = c_type_is_pointer(memory_type) ||
                          memory_type == C_I64 || memory_type == C_U64;
        lower_cmpxchg8 = memory_type == C_U8 || memory_type == C_CHAR;
        lower_cmpxchg16 = memory_type == C_I16 || memory_type == C_U16;
        bool output_type_ok =
            asm_output &&
            (lower_cmpxchg64
                 ? (asm_output->type == C_I64 || asm_output->type == C_U64 ||
                    c_type_is_pointer(asm_output->type) ||
                    asm_output->type == C_I32 || asm_output->type == C_U32)
             : lower_cmpxchg8
                 ? (asm_output->type == C_U8 || asm_output->type == C_CHAR ||
                    asm_output->type == C_U32 || asm_output->type == C_I32 ||
                    c_type_is_pointer(asm_output->type))
             : lower_cmpxchg16
                 ? (asm_output->type == C_I16 || asm_output->type == C_U16 ||
                    asm_output->type == C_U32 || asm_output->type == C_I32 ||
                    c_type_is_pointer(asm_output->type))
                 : (asm_output->type == C_I32 || asm_output->type == C_U32 ||
                    c_type_is_pointer(asm_output->type)));
        bool new_type_ok =
            lower_cmpxchg64
                ? (new_value.type == C_I64 || new_value.type == C_U64 ||
                   c_type_is_pointer(new_value.type) ||
                   new_value.type == C_I32 || new_value.type == C_U32)
            : lower_cmpxchg8
                ? (new_value.type == C_U8 || new_value.type == C_CHAR ||
                   new_value.type == C_U32 || new_value.type == C_I32 ||
                   c_type_is_pointer(new_value.type))
            : lower_cmpxchg16
                ? (new_value.type == C_I16 || new_value.type == C_U16 ||
                   new_value.type == C_U32 || new_value.type == C_I32 ||
                   c_type_is_pointer(new_value.type))
                : (new_value.type == C_I32 || new_value.type == C_U32 ||
                   c_type_is_pointer(new_value.type));
        if (!asm_condition_output || asm_condition_output->type != C_U8 ||
            !asm_output || !output_type_ok || !new_type_ok ||
            asm_output->slot >= (size_t)INT64_MAX)
          return defer_unsupported_asm(p, open, assembly);
        asm_memory_output_value = memory_address;
        asm_memory_register_input_value = lower_local_load(p, asm_output);
        asm_cmpxchg_new_value = new_value.value;
        if (asm_memory_register_input_value < 0 ||
            !substitute_inline_asm_named_operands(
                &assembly, &assembly_len, &assembly_cap, memory_name,
                memory_segment & NYIR_INST_F_MEM_SEG_GS   ? "%gs:(%rcx)"
                : memory_segment & NYIR_INST_F_MEM_SEG_FS ? "%fs:(%rcx)"
                                                          : "(%rcx)",
                (inline_asm_named_value_t[]){
                    {.name = new_name,
                     .replacement = lower_cmpxchg64   ? "%rdx"
                                    : lower_cmpxchg8  ? "%dl"
                                    : lower_cmpxchg16 ? "%dx"
                                                      : "%edx"},
                    {.name = old_name,
                     .replacement = lower_cmpxchg64   ? "%rax"
                                    : lower_cmpxchg8  ? "%al"
                                    : lower_cmpxchg16 ? "%ax"
                                                      : "%eax"}},
                2))
          return defer_unsupported_asm(p, open, assembly);
        if (lower_cmpxchg64) {
          for (size_t i = 0; i + 1 < assembly_len; ++i)
            if (assembly[i] == '%' && assembly[i + 1] == '%') {
              memmove(&assembly[i], &assembly[i + 1], assembly_len - i - 1);
              --assembly_len;
            }
        }
        lower_cmpxchg_outputs = true;
      }
      if (!lower_cmpxchg_outputs) {
        c_parser_t read_write_probe = *p;
        size_t register_constraint_end = read_write_probe.pos;
        size_t memory_constraint_end = read_write_probe.pos;
        ny_ctok_t register_output_name = {0};
        bool named_register_output = parse_named_inline_asm_operand(
            &read_write_probe, &register_output_name);
        register_constraint_end = read_write_probe.pos;
        bool register_constraint_q = inline_asm_string_sequence_is(
            &read_write_probe, "+q", &register_constraint_end);
        bool read_write_register_memory =
            named_register_output &&
            (register_constraint_q ||
             inline_asm_string_sequence_is(&read_write_probe, "+r",
                                           &register_constraint_end));
        if (read_write_register_memory) {
          read_write_probe.pos = register_constraint_end;
          read_write_register_memory =
              tok_is(&read_write_probe, read_write_probe.pos, "(") &&
              read_write_probe.pos + 3 < read_write_probe.token_count &&
              read_write_probe.tokens[read_write_probe.pos + 1].kind ==
                  NY_CTOK_IDENT &&
              tok_is(&read_write_probe, read_write_probe.pos + 2, ")") &&
              tok_is(&read_write_probe, read_write_probe.pos + 3, ",");
          if (read_write_register_memory) {
            register_output_name =
                read_write_probe.tokens[read_write_probe.pos + 1];
            read_write_probe.pos += 4;
            if (accept(&read_write_probe, "[")) {
              read_write_register_memory =
                  read_write_probe.pos + 1 < read_write_probe.token_count &&
                  read_write_probe.tokens[read_write_probe.pos].kind ==
                      NY_CTOK_IDENT &&
                  tok_is(&read_write_probe, read_write_probe.pos + 1, "]");
              if (read_write_register_memory)
                read_write_probe.pos += 2;
            }
          }
        }
        if (read_write_register_memory) {
          memory_constraint_end = read_write_probe.pos;
          read_write_register_memory = inline_asm_string_sequence_is(
              &read_write_probe, "+m", &memory_constraint_end);
        }
        if (read_write_register_memory) {
          read_write_probe.pos = memory_constraint_end;
          if (!expect(&read_write_probe, "("))
            return defer_unsupported_asm(p, open, assembly);
          c_value_t memory_lvalue = parse_expr(&read_write_probe, 1);
          const c_lvalue_t *recorded =
              memory_lvalue.value >= 0
                  ? find_lvalue(&read_write_probe, memory_lvalue.value)
                  : NULL;
          int memory_address =
              recorded
                  ? lower_asm_memory_output_address(&read_write_probe, recorded)
                  : -1;
          if (memory_address < 0 || !expect(&read_write_probe, ")") ||
              !expect(&read_write_probe, ":") ||
              !expect(&read_write_probe, ":"))
            return defer_unsupported_asm(p, open, assembly);
          size_t clobber_end = read_write_probe.pos;
          if (!inline_asm_clobber_list_is(&read_write_probe, &clobber_end))
            return defer_unsupported_asm(p, open, assembly);
          read_write_probe.pos = clobber_end;
          asm_output = find_local(&read_write_probe, register_output_name);
          int xchg_width = inline_asm_xchg_width(assembly, assembly_len);
          int xadd_width = inline_asm_xadd_width(assembly, assembly_len);
          static const char named_xaddq_template[] = "xaddq %[tmp], %[var]";
          if (xadd_width < 0 &&
              assembly_len == sizeof(named_xaddq_template) - 1 &&
              memcmp(assembly, named_xaddq_template,
                     sizeof(named_xaddq_template) - 1) == 0) {
            static const char positional_xaddq_template[] = "xaddq %q0, %1";
            unsigned char *normalized =
                (unsigned char *)strdup(positional_xaddq_template);
            if (!normalized) {
              fail(p, "out of memory normalizing xadd inline assembly");
              return false;
            }
            free(assembly);
            assembly = normalized;
            assembly_len = sizeof(positional_xaddq_template) - 1;
            assembly_cap = assembly_len + 1;
            xadd_width = 3;
            asm_memory_register_xadd_unlocked = true;
          }
          static const char locked_xaddq_unqualified[] = "lock xaddq %0, %1";
          static const char xaddq_unqualified[] = "xaddq %0, %1";
          if (xadd_width < 0 &&
              inline_asm_template_equals(assembly, assembly_len,
                                         locked_xaddq_unqualified)) {
            static const char normalized_template[] = "lock xaddq %q0, %1";
            unsigned char *normalized =
                (unsigned char *)strdup(normalized_template);
            if (!normalized) {
              fail(p, "out of memory normalizing xadd inline assembly");
              return false;
            }
            free(assembly);
            assembly = normalized;
            assembly_len = sizeof(normalized_template) - 1;
            assembly_cap = assembly_len + 1;
            xadd_width = 3;
          } else if (xadd_width < 0 &&
                     inline_asm_template_equals(assembly, assembly_len,
                                                xaddq_unqualified)) {
            static const char normalized_template[] = "xaddq %q0, %1";
            unsigned char *normalized =
                (unsigned char *)strdup(normalized_template);
            if (!normalized) {
              fail(p, "out of memory normalizing xadd inline assembly");
              return false;
            }
            free(assembly);
            assembly = normalized;
            assembly_len = sizeof(normalized_template) - 1;
            assembly_cap = assembly_len + 1;
            xadd_width = 3;
            asm_memory_register_xadd_unlocked = true;
          }
          static const char *const named_gs_xadd_templates[] = {
              "xaddb %[tmp], %[var]", "xaddw %[tmp], %[var]",
              "xaddl %[tmp], %[var]", "xaddq %[tmp], %[var]"};
          static const char *const named_percpu_xadd_templates[] = {
              "xaddb %[tmp], %%gs:%[var]", "xaddw %[tmp], %%gs:%[var]",
              "xaddl %[tmp], %%gs:%[var]", "xaddq %[tmp], %%gs:%[var]"};
          static const char *const positional_percpu_xadd_templates[] = {
              "xaddb %b0, %1", "xaddw %w0, %1", "xaddl %0, %1",
              "xaddq %q0, %1"};
          if (xadd_width < 0 &&
              inline_asm_statement_has_token(p, open, "__seg_gs")) {
            for (int width = 0; width < 4; ++width) {
              size_t named_len = strlen(named_gs_xadd_templates[width]);
              if (assembly_len != named_len ||
                  memcmp(assembly, named_gs_xadd_templates[width], named_len) !=
                      0)
                continue;
              unsigned char *normalized = (unsigned char *)strdup(
                  positional_percpu_xadd_templates[width]);
              if (!normalized) {
                fail(p, "out of memory normalizing per-CPU xadd assembly");
                return false;
              }
              free(assembly);
              assembly = normalized;
              assembly_len = strlen(positional_percpu_xadd_templates[width]);
              assembly_cap = assembly_len + 1;
              xadd_width = width;
              asm_memory_register_xadd_unlocked = true;
              asm_memory_register_xadd_gs = true;
              break;
            }
          }
          if (xadd_width < 0) {
            for (int width = 0; width < 4; ++width) {
              size_t named_len = strlen(named_percpu_xadd_templates[width]);
              if (assembly_len != named_len ||
                  memcmp(assembly, named_percpu_xadd_templates[width],
                         named_len) != 0)
                continue;
              unsigned char *normalized = (unsigned char *)strdup(
                  positional_percpu_xadd_templates[width]);
              if (!normalized) {
                fail(p, "out of memory normalizing per-CPU xadd assembly");
                return false;
              }
              free(assembly);
              assembly = normalized;
              assembly_len = strlen(positional_percpu_xadd_templates[width]);
              assembly_cap = assembly_len + 1;
              xadd_width = width;
              asm_memory_register_xadd_unlocked = true;
              asm_memory_register_xadd_gs = true;
              break;
            }
          }
          bool pointer_xchg =
              asm_output && xchg_width >= 0 &&
              c_type_is_pointer(asm_output->type) &&
              c_type_is_pointer(recorded->type) &&
              (c_pointer_compatible(asm_output->type, recorded->type) ||
               (asm_output->type == C_FUNC_PTR &&
                recorded->type == C_FUNC_PTR));
          if (!asm_output || asm_output->array_rank || asm_output->is_const ||
              ((!c_type_is_integer(asm_output->type) && !pointer_xchg) ||
               (xchg_width < 0 && xadd_width < 0 &&
                (asm_output->type != C_I32 || register_constraint_q)) ||
               (xadd_width >= 0 && !c_type_is_integer(asm_output->type))) ||
              asm_output->slot >= (size_t)INT64_MAX ||
              (register_constraint_q && xchg_width != 0 && xadd_width != 0))
            return defer_unsupported_asm(p, open, assembly);
          asm_memory_output_value = memory_address;
          asm_memory_register_input_value =
              lower_local_load(&read_write_probe, asm_output);
          if (asm_memory_register_input_value < 0)
            return defer_unsupported_asm(p, open, assembly);
          immediate_values[0] = "%edx";
          if (xadd_width >= 0) {
            asm_memory_register_imm = -50;
            if ((register_constraint_q && xadd_width != 0) ||
                !substitute_inline_asm_register_memory(&assembly, &assembly_len,
                                                       &assembly_cap))
              return defer_unsupported_asm(p, open, assembly);
          } else if (xchg_width >= 0) {
            bool is_signed =
                asm_output->type == C_CHAR || asm_output->type == C_I16 ||
                asm_output->type == C_I32 || asm_output->type == C_I64;
            asm_memory_register_imm =
                xchg_width == 0   ? (is_signed ? -41 : -40)
                : xchg_width == 1 ? (is_signed ? -43 : -42)
                : xchg_width == 2 ? (is_signed ? -45 : -44)
                                  : -46;
            if (!substitute_inline_asm_register_memory(&assembly, &assembly_len,
                                                       &assembly_cap))
              return defer_unsupported_asm(p, open, assembly);
          } else if (!substitute_inline_asm_immediate(&assembly, &assembly_len,
                                                      &assembly_cap,
                                                      immediate_values, 1, 1)) {
            return defer_unsupported_asm(p, open, assembly);
          }
          p->pos = read_write_probe.pos;
          lower_memory_register_output = true;
        }
        if (lower_memory_register_output) {
          /*
           * The register and memory operands are both read-write outputs.
           */
        } else {
          c_parser_t memory_probe = *p;
          ny_ctok_t memory_constraint_name = {0};
          size_t memory_constraint_end = memory_probe.pos;
          bool named_memory_output =
              parse_named_inline_asm_operand(&memory_probe,
                                             &memory_constraint_name) &&
              memory_constraint_name.kind == NY_CTOK_IDENT &&
              inline_asm_has_named_operand(assembly, assembly_len) &&
              (inline_asm_string_sequence_is(&memory_probe, "+m",
                                             &memory_constraint_end) ||
               (memmem(assembly, assembly_len, "%%gs:", sizeof("%%gs:") - 1) &&
                inline_asm_string_sequence_is(&memory_probe, "=m",
                                              &memory_constraint_end)));
          size_t unnamed_memory_constraint_end = p->pos;
          bool unnamed_memory_output =
              !named_memory_output &&
              (inline_asm_string_sequence_is(p, "+m",
                                             &unnamed_memory_constraint_end) ||
               inline_asm_string_sequence_is(p, "=m",
                                             &unnamed_memory_constraint_end));
          if (named_memory_output) {
            p->pos = memory_constraint_end;
            if (!expect(p, "("))
              return defer_unsupported_asm(p, open, assembly);
            c_value_t output_lvalue = parse_expr(p, 1);
            const c_lvalue_t *recorded =
                output_lvalue.value >= 0 ? find_lvalue(p, output_lvalue.value)
                                         : NULL;
            if (!recorded || !expect(p, ")"))
              return defer_unsupported_asm(p, open, assembly);
            unsigned segment = recorded->segment_flags;
            asm_memory_output_value =
                lower_asm_memory_output_address(p, recorded);
            if (asm_memory_output_value < 0)
              return defer_unsupported_asm(p, open, assembly);
            bool carry_condition_output = false;
            if (accept(p, ",")) {
              size_t condition_constraint_end = p->pos;
              ny_ctok_t condition_name;
              carry_condition_output = inline_asm_string_sequence_is(
                  p, "=@ccc", &condition_constraint_end);
              if (!carry_condition_output &&
                  !inline_asm_string_sequence_is(p, "=@cce",
                                                 &condition_constraint_end) &&
                  !inline_asm_string_sequence_is(p, "=@ccz",
                                                 &condition_constraint_end))
                return defer_unsupported_asm(p, open, assembly);
              p->pos = condition_constraint_end;
              if (!expect(p, "(") || !parse_name(p, &condition_name) ||
                  !expect(p, ")"))
                return defer_unsupported_asm(p, open, assembly);
              asm_condition_output = find_local(p, condition_name);
              if (!asm_condition_output || asm_condition_output->array_rank ||
                  asm_condition_output->is_const ||
                  asm_condition_output->type != C_U8)
                return defer_unsupported_asm(p, open, assembly);
              lower_named_memory_condition_output = true;
            }
            inline_asm_named_value_t named_inputs[1];
            size_t named_input_count = 0;
            if (lower_named_memory_condition_output) {
              if (!accept(p, ":"))
                return defer_unsupported_asm(p, open, assembly);
              if (!accept(p, ":")) {
                ny_ctok_t input_name = {0};
                size_t input_constraint_end = p->pos;
                bool supported_constraint =
                    parse_named_inline_asm_operand(p, &input_name) &&
                    input_name.kind == NY_CTOK_IDENT &&
                    (inline_asm_string_sequence_is(p, "Ir",
                                                   &input_constraint_end) ||
                     inline_asm_string_sequence_is(p, "er",
                                                   &input_constraint_end) ||
                     inline_asm_string_sequence_is(p, "r",
                                                   &input_constraint_end) ||
                     inline_asm_string_sequence_is(p, "ri",
                                                   &input_constraint_end) ||
                     inline_asm_string_sequence_is(p, "ir",
                                                   &input_constraint_end));
                if (!supported_constraint)
                  return defer_unsupported_asm(p, open, assembly);
                p->pos = input_constraint_end;
                if (!expect(p, "("))
                  return defer_unsupported_asm(p, open, assembly);
                c_value_t input = parse_expr(p, 1);
                if (input.value < 0 || !expect(p, ")") || !accept(p, ":"))
                  return defer_unsupported_asm(p, open, assembly);
                asm_memory_register_input_value = input.value;
                named_inputs[named_input_count++] = (inline_asm_named_value_t){
                    .name = input_name,
                    .replacement =
                        inline_asm_integer_register_name(input.type)};
              }
              size_t clobber_end = p->pos;
              if (!inline_asm_clobber_list_is(p, &clobber_end))
                return defer_unsupported_asm(p, open, assembly);
              p->pos = clobber_end;
              if (!append_asm_bytes(
                      &assembly, &assembly_len, &assembly_cap,
                      carry_condition_output
                          ? "\n\tsetc %cl\n\tmovzbl %cl, %eax"
                          : "\n\tsete %cl\n\tmovzbl %cl, %eax",
                      carry_condition_output
                          ? sizeof("\n\tsetc %cl\n\tmovzbl %cl, %eax") - 1
                          : sizeof("\n\tsete %cl\n\tmovzbl %cl, %eax") - 1))
                return defer_unsupported_asm(p, open, assembly);
            } else if (accept(p, ":")) {
              ny_ctok_t input_name = {0};
              size_t input_constraint_end = p->pos;
              bool supported_constraint =
                  parse_named_inline_asm_operand(p, &input_name) &&
                  input_name.kind == NY_CTOK_IDENT &&
                  (inline_asm_string_sequence_is(p, "re",
                                                 &input_constraint_end) ||
                   inline_asm_string_sequence_is(p, "ri",
                                                 &input_constraint_end) ||
                   inline_asm_string_sequence_is(p, "ir",
                                                 &input_constraint_end) ||
                   inline_asm_string_sequence_is(p, "Ir",
                                                 &input_constraint_end) ||
                   inline_asm_string_sequence_is(p, "r",
                                                 &input_constraint_end) ||
                   inline_asm_string_sequence_is(p, "qi",
                                                 &input_constraint_end));
              if (!supported_constraint)
                return defer_unsupported_asm(p, open, assembly);
              p->pos = input_constraint_end;
              if (!expect(p, "("))
                return defer_unsupported_asm(p, open, assembly);
              c_value_t input = parse_expr(p, 1);
              if (input.value < 0 || !expect(p, ")"))
                return defer_unsupported_asm(p, open, assembly);
              asm_memory_register_input_value = input.value;
              named_inputs[named_input_count++] = (inline_asm_named_value_t){
                  .name = input_name,
                  .replacement = inline_asm_integer_register_name(input.type)};
            }
            if (accept(p, ":")) {
              size_t clobber_end = p->pos;
              if (!inline_asm_clobber_list_is(p, &clobber_end))
                return defer_unsupported_asm(p, open, assembly);
              p->pos = clobber_end;
            }
            if (!substitute_inline_asm_named_operands(
                    &assembly, &assembly_len, &assembly_cap,
                    memory_constraint_name,
                    segment & NYIR_INST_F_MEM_SEG_GS   ? "%gs:(%rax)"
                    : segment & NYIR_INST_F_MEM_SEG_FS ? "%fs:(%rax)"
                                                       : "(%rax)",
                    named_inputs, named_input_count))
              return defer_unsupported_asm(p, open, assembly);
            lower_named_memory_output = true;
          } else if (unnamed_memory_output) {
            p->pos = unnamed_memory_constraint_end;
            if (!expect(p, "("))
              return defer_unsupported_asm(p, open, assembly);
            c_value_t output_lvalue = parse_expr(p, 1);
            const c_lvalue_t *recorded =
                output_lvalue.value >= 0 ? find_lvalue(p, output_lvalue.value)
                                         : NULL;
            if (!recorded || !expect(p, ")"))
              return defer_unsupported_asm(p, open, assembly);
            asm_memory_output_value =
                lower_asm_memory_output_address(p, recorded);
            if (asm_memory_output_value < 0)
              return defer_unsupported_asm(p, open, assembly);
            if (accept(p, ":")) {
              bool clobbers = accept(p, ":");
              if (!clobbers && !tok_is(p, p->pos, ")"))
                return defer_unsupported_asm(p, open, assembly);
              if (clobbers && !tok_is(p, p->pos, ")")) {
                size_t clobber_end = p->pos;
                if (!inline_asm_clobber_list_matches(p, &clobber_end, true))
                  return defer_unsupported_asm(p, open, assembly);
                p->pos = clobber_end;
              }
            }
            if (!tok_is(p, p->pos, ")"))
              return defer_unsupported_asm(p, open, assembly);
            immediate_values[0] = "(%rax)";
            if (!substitute_inline_asm_immediate(&assembly, &assembly_len,
                                                 &assembly_cap,
                                                 immediate_values, 1, SIZE_MAX))
              return defer_unsupported_asm(p, open, assembly);
            unescape_inline_asm_percents(assembly, &assembly_len);
            lower_named_memory_output = true;
          } else {
            c_parser_t output_probe = *p;
            ny_ctok_t output_constraint_name = {0};
            size_t output_constraint_end = output_probe.pos;
            bool empty_read_write_memory_output =
                !assembly_len &&
                inline_asm_string_sequence_is(&output_probe, "+rm",
                                              &output_constraint_end);
            bool named_register_output =
                parse_named_inline_asm_operand(&output_probe,
                                               &output_constraint_name) &&
                output_constraint_name.kind == NY_CTOK_IDENT &&
                inline_asm_has_named_operand(assembly, assembly_len) &&
                (inline_asm_string_sequence_is(&output_probe, "=r",
                                               &output_constraint_end) ||
                 inline_asm_string_sequence_is(&output_probe, "=q",
                                               &output_constraint_end));
            if (empty_read_write_memory_output) {
              p->pos = output_constraint_end;
              if (!expect(p, "("))
                return defer_unsupported_asm(p, open, assembly);
              c_value_t output_lvalue = parse_expr(p, 1);
              const c_lvalue_t *recorded =
                  output_lvalue.value >= 0 ? find_lvalue(p, output_lvalue.value)
                                           : NULL;
              if (!recorded || !expect(p, ")") ||
                  !discard_lvalue_read(p, recorded))
                return defer_unsupported_asm(p, open, assembly);
              lower_empty_memory_barrier = true;
            } else if (named_register_output) {
              bool byte_register_output = inline_asm_string_sequence_is(
                  &output_probe, "=q", &output_constraint_end);
              p->pos = output_constraint_end;
              ny_ctok_t output_name;
              if (!expect(p, "(") || !parse_name(p, &output_name) ||
                  !expect(p, ")") || !expect(p, ":"))
                return defer_unsupported_asm(p, open, assembly);
              asm_output = find_local(p, output_name);
              if (!asm_output || asm_output->array_rank ||
                  asm_output->is_const ||
                  (!c_type_is_pointer(asm_output->type) &&
                   !c_type_is_integer(asm_output->type)) ||
                  asm_output->slot >= (size_t)INT64_MAX)
                return defer_unsupported_asm(p, open, assembly);
              inline_asm_named_value_t named_inputs[16];
              size_t named_input_count = 0;
              for (;;) {
                if (named_input_count ==
                    sizeof(named_inputs) / sizeof(named_inputs[0]))
                  return defer_unsupported_asm(p, open, assembly);
                ny_ctok_t input_name = {0};
                size_t input_constraint_end = p->pos;
                if (!parse_named_inline_asm_operand(p, &input_name) ||
                    input_name.kind != NY_CTOK_IDENT)
                  return defer_unsupported_asm(p, open, assembly);
                bool memory_input = inline_asm_string_sequence_is(
                    p, "m", &input_constraint_end);
                bool immediate_input =
                    !memory_input && inline_asm_string_sequence_is(
                                         p, "i", &input_constraint_end);
                if (!memory_input && !immediate_input)
                  return defer_unsupported_asm(p, open, assembly);
                p->pos = input_constraint_end;
                if (!expect(p, "("))
                  return defer_unsupported_asm(p, open, assembly);
                char *immediate = immediate_storage[named_input_count];
                size_t immediate_capacity = sizeof(immediate_storage[0]);
                if (memory_input) {
                  c_value_t memory_value = parse_expr(p, 1);
                  const c_lvalue_t *memory_lvalue =
                      memory_value.value >= 0
                          ? find_asm_memory_lvalue(p, memory_value.value)
                          : NULL;
                  if (memory_value.value < 0 || !memory_lvalue ||
                      memory_lvalue->address < 0 || !expect(p, ")"))
                    return defer_unsupported_asm(p, open, assembly);
                  asm_memory_input_value = memory_lvalue->address;
                  memcpy(immediate, "%P1", sizeof("%P1"));
                  lower_named_register_memory_input = true;
                  named_inputs[named_input_count++] =
                      (inline_asm_named_value_t){.name = input_name,
                                                 .replacement = immediate};
                  if (!accept(p, ","))
                    break;
                  continue;
                }
                if (accept(p, "&")) {
                  bool parenthesized = accept(p, "(");
                  ny_ctok_t global_name;
                  if (!parse_name(p, &global_name))
                    return defer_unsupported_asm(p, open, assembly);
                  c_global_t *global = find_global(p, global_name);
                  if (!global || (parenthesized && !expect(p, ")")) ||
                      !expect(p, ")"))
                    return defer_unsupported_asm(p, open, assembly);
                  int written =
                      snprintf(immediate, immediate_capacity, "%.*s",
                               (int)global_name.len, global_name.start);
                  if (written < 0 || (size_t)written >= immediate_capacity)
                    return defer_unsupported_asm(p, open, assembly);
                } else if (current(p)->kind == NY_CTOK_STRING) {
                  size_t global_count = p->global_count;
                  c_value_t string_value = lower_string_literal(p);
                  if (string_value.value < 0 ||
                      p->global_count != global_count + 1)
                    return defer_unsupported_asm(p, open, assembly);
                  ny_ctok_t string_name = p->globals[global_count].name;
                  int written =
                      snprintf(immediate, immediate_capacity, "__nyc_data_%.*s",
                               (int)string_name.len, string_name.start);
                  if (written < 0 || (size_t)written >= immediate_capacity ||
                      !expect(p, ")"))
                    return defer_unsupported_asm(p, open, assembly);
                } else {
                  size_t value_pos = p->pos;
                  int64_t value = 0;
                  bool previous_asm_constant_context =
                      p->inline_asm_constant_context;
                  p->inline_asm_constant_context = true;
                  bool constant = parse_global_const_expr(p, &p->pos, &value);
                  p->inline_asm_constant_context =
                      previous_asm_constant_context;
                  if (!constant) {
                    p->pos = value_pos;
                    if (current(p)->kind == NY_CTOK_IDENT) {
                      c_local_t *constant_local = find_local(p, *current(p));
                      if (constant_local &&
                          constant_local->has_integer_constant &&
                          !constant_local->address_taken &&
                          !constant_local->is_volatile &&
                          !constant_local->static_name.len) {
                        value = constant_local->integer_constant;
                        ++p->pos;
                        constant = true;
                      }
                    }
                  }
                  if (!constant || !expect(p, ")"))
                    return defer_unsupported_asm(p, open, assembly);
                  int written = snprintf(immediate, immediate_capacity, "%lld",
                                         (long long)value);
                  if (written < 0 || (size_t)written >= immediate_capacity)
                    return defer_unsupported_asm(p, open, assembly);
                }
                named_inputs[named_input_count++] = (inline_asm_named_value_t){
                    .name = input_name, .replacement = immediate};
                if (!accept(p, ","))
                  break;
              }
              if (accept(p, ":")) {
                size_t clobber_end = p->pos;
                if (!inline_asm_clobber_list_is(p, &clobber_end))
                  return defer_unsupported_asm(p, open, assembly);
                p->pos = clobber_end;
              }
              const char *register_replacement =
                  byte_register_output ? "%al"
                  : assembly_len >= sizeof("movw ") - 1 &&
                          memcmp(assembly, "movw ", sizeof("movw ") - 1) == 0
                      ? "%ax"
                  : assembly_len >= sizeof("movl ") - 1 &&
                          memcmp(assembly, "movl ", sizeof("movl ") - 1) == 0
                      ? "%eax"
                      : "%rax";
              if (!substitute_inline_asm_named_operands(
                      &assembly, &assembly_len, &assembly_cap,
                      output_constraint_name, register_replacement,
                      named_inputs, named_input_count))
                return defer_unsupported_asm(p, open, assembly);
              lower_named_register_output = true;
            } else {
              c_parser_t condition_probe = *p;
              size_t condition_constraint_end = condition_probe.pos;
              bool carry_flag_output = inline_asm_string_sequence_is(
                  &condition_probe, "=@ccc", &condition_constraint_end);
              c_parser_t nonzero_condition_probe = *p;
              size_t nonzero_condition_constraint_end =
                  nonzero_condition_probe.pos;
              bool nonzero_flag_output = inline_asm_string_sequence_is(
                  &nonzero_condition_probe, "=@ccnz",
                  &nonzero_condition_constraint_end);
              c_parser_t simple_output_probe = *p;
              size_t simple_output_end = simple_output_probe.pos;
              bool simple_register_output =
                  parse_inline_asm_operand_name(&simple_output_probe) &&
                  inline_asm_string_sequence_is(&simple_output_probe, "=r",
                                                &simple_output_end);
              size_t empty_input_constraint_end = p->pos;
              if (!assembly_len && inline_asm_string_sequence_is(
                                       p, "r", &empty_input_constraint_end)) {
                p->pos = empty_input_constraint_end;
                if (!expect(p, "("))
                  return defer_unsupported_asm(p, open, assembly);
                c_value_t input = parse_expr(p, 1);
                if (input.value < 0 ||
                    (!c_type_is_integer(input.type) &&
                     !c_type_is_pointer(input.type)) ||
                    !expect(p, ")") || !expect(p, ":"))
                  return defer_unsupported_asm(p, open, assembly);
                size_t clobber_end = p->pos;
                if (!inline_asm_clobber_list_is(p, &clobber_end))
                  return defer_unsupported_asm(p, open, assembly);
                p->pos = clobber_end;
                lower_empty_memory_barrier = true;
              } else if (assembly_len == sizeof("lea (%%rip), %0\n\tmov %%rsp, "
                                                "%1\n\tmov %%rbp, %2\n\t") -
                                             1 &&
                         memcmp(assembly,
                                "lea (%%rip), %0\n\tmov %%rsp, %1\n\tmov "
                                "%%rbp, %2\n\t",
                                assembly_len) == 0) {
                for (size_t output_index = 0; output_index < 3;
                     ++output_index) {
                  size_t constraint_end = p->pos;
                  if (!inline_asm_string_sequence_is(p, "=r", &constraint_end))
                    return defer_unsupported_asm(p, open, assembly);
                  p->pos = constraint_end;
                  if (!expect(p, "("))
                    return defer_unsupported_asm(p, open, assembly);
                  c_value_t output = parse_expr(p, 1);
                  const c_lvalue_t *lvalue =
                      output.value >= 0 ? find_lvalue(p, output.value) : NULL;
                  if (!lvalue || lvalue->bitfield ||
                      c_type_size(p->abi, lvalue->type) != 8 ||
                      (!c_type_is_integer(lvalue->type) &&
                       !c_type_is_pointer(lvalue->type)) ||
                      !expect(p, ")"))
                    return defer_unsupported_asm(p, open, assembly);
                  x86_snapshot_outputs[output_index] = *lvalue;
                  if (output_index != 2 && !expect(p, ","))
                    return defer_unsupported_asm(p, open, assembly);
                }
                lower_x86_register_snapshot = true;
              } else if (carry_flag_output || nonzero_flag_output) {
                static const char *const random_instructions[] = {"rdrand",
                                                                  "rdseed"};
                const char *random_instruction = NULL;
                for (size_t i = 0; i < sizeof(random_instructions) /
                                           sizeof(random_instructions[0]);
                     ++i) {
                  char expected[24];
                  int length = snprintf(expected, sizeof(expected),
                                        "%s %%[out]", random_instructions[i]);
                  if (length > 0 && (size_t)length == assembly_len &&
                      memcmp(assembly, expected, assembly_len) == 0) {
                    random_instruction = random_instructions[i];
                    break;
                  }
                }
                if (carry_flag_output && random_instruction) {
                  p->pos = condition_constraint_end;
                  ny_ctok_t condition_name;
                  if (!expect(p, "(") || !parse_name(p, &condition_name) ||
                      !expect(p, ")") || !expect(p, ",")) {
                    fail(p, "invalid RDRAND/RDSEED status output");
                    return false;
                  }
                  asm_condition_output = find_local(p, condition_name);
                  ny_ctok_t value_operand;
                  if (!asm_condition_output ||
                      asm_condition_output->array_rank ||
                      asm_condition_output->is_const ||
                      asm_condition_output->type != C_U8 ||
                      !parse_named_inline_asm_operand(p, &value_operand) ||
                      !inline_asm_name_matches(
                          value_operand, (const unsigned char *)"out", 3) ||
                      !inline_asm_string_sequence_is(p, "=r", &p->pos) ||
                      !expect(p, "(")) {
                    fail(p, "invalid RDRAND/RDSEED value output");
                    return false;
                  }
                  c_value_t output_value = parse_expr(p, 1);
                  const c_lvalue_t *output_lvalue =
                      output_value.value >= 0
                          ? find_lvalue(p, output_value.value)
                          : NULL;
                  if (!output_lvalue || output_lvalue->address < 0 ||
                      (output_value.type != C_U64 &&
                       output_value.type != C_I64) ||
                      !expect(p, ")")) {
                    fail(p, "RDRAND/RDSEED requires a 64-bit lvalue output");
                    return false;
                  }
                  if (accept(p, ":")) {
                    size_t clobber_end = p->pos;
                    if (!inline_asm_clobber_list_is(p, &clobber_end)) {
                      fail(p, "invalid RDRAND/RDSEED clobber list");
                      return false;
                    }
                    p->pos = clobber_end;
                  }
                  if (!expect(p, ")") || !expect(p, ";")) {
                    fail(p, "invalid RDRAND/RDSEED asm terminator");
                    return false;
                  }
                  int result = emit_value(
                      p, (nyr_inst_t){.op = NYIR_INLINE_ASM,
                                      .dst = -1,
                                      .a = output_lvalue->address,
                                      .b = -1,
                                      .c = -1,
                                      .d = -1,
                                      .imm = -132,
                                      .symbol = random_instruction,
                                      .scalar_class = NYIR_SCALAR_I64_RAW,
                                      .effects = NYIR_EFFECT_WRITE_MEMORY |
                                                 NYIR_EFFECT_CONTROL |
                                                 NYIR_EFFECT_VOLATILE});
                  free(assembly);
                  if (result < 0) {
                    fail(p, "failed to lower RDRAND/RDSEED instruction");
                    return false;
                  }
                  return lower_local_store(p, asm_condition_output, result);
                }
                p->pos = carry_flag_output ? condition_constraint_end
                                           : nonzero_condition_constraint_end;
                ny_ctok_t output_name;
                if (!expect(p, "(") || !parse_name(p, &output_name) ||
                    !expect(p, ")") || !expect(p, ":"))
                  return defer_unsupported_asm(p, open, assembly);
                asm_condition_output = find_local(p, output_name);
                if (!asm_condition_output || asm_condition_output->array_rank ||
                    asm_condition_output->is_const ||
                    asm_condition_output->type != C_U8)
                  return defer_unsupported_asm(p, open, assembly);

                size_t memory_constraint_end = p->pos;
                if (!inline_asm_string_sequence_is(p, "m",
                                                   &memory_constraint_end))
                  return defer_unsupported_asm(p, open, assembly);
                p->pos = memory_constraint_end;
                if (!expect(p, "("))
                  return defer_unsupported_asm(p, open, assembly);
                c_value_t memory_input = parse_expr(p, 1);
                const c_lvalue_t *memory_lvalue =
                    memory_input.value >= 0
                        ? find_asm_memory_lvalue(p, memory_input.value)
                        : NULL;
                if (!memory_lvalue || memory_lvalue->address < 0 ||
                    !expect(p, ")"))
                  return defer_unsupported_asm(p, open, assembly);
                asm_memory_input_value = memory_lvalue->address;
                memory_operand_index = 1;
                immediate_values[0] = "0";
                immediate_values[1] = NULL;
                immediate_count = 2;

                if (!accept(p, ","))
                  return defer_unsupported_asm(p, open, assembly);
                size_t register_constraint_end = p->pos;
                const char *register_constraint =
                    nonzero_flag_output ? "i" : "Ir";
                if (!inline_asm_string_sequence_is(p, register_constraint,
                                                   &register_constraint_end))
                  return defer_unsupported_asm(p, open, assembly);
                p->pos = register_constraint_end;
                if (!expect(p, "("))
                  return defer_unsupported_asm(p, open, assembly);
                c_value_t register_input = parse_expr(p, 1);
                if (register_input.value < 0 ||
                    !c_type_is_integer(register_input.type) || !expect(p, ")"))
                  return defer_unsupported_asm(p, open, assembly);
                asm_memory_register_input_value = register_input.value;
                int written =
                    snprintf(immediate_storage[2], sizeof(immediate_storage[2]),
                             nonzero_flag_output ? "%%%%dl" : "%%%%rdx");
                if (written < 0 ||
                    (size_t)written >= sizeof(immediate_storage[2]))
                  return defer_unsupported_asm(p, open, assembly);
                immediate_values[2] = immediate_storage[2];
                immediate_count = 3;
                if (!substitute_inline_asm_immediate(
                        &assembly, &assembly_len, &assembly_cap,
                        immediate_values, immediate_count,
                        memory_operand_index) ||
                    !append_asm_bytes(
                        &assembly, &assembly_len, &assembly_cap,
                        nonzero_flag_output
                            ? "\n\tsetne %al\n\tmovzbl %al, %eax"
                            : "\n\tsetc %al\n\tmovzbl %al, %eax",
                        nonzero_flag_output
                            ? sizeof("\n\tsetne %al\n\tmovzbl %al, %eax") - 1
                            : sizeof("\n\tsetc %al\n\tmovzbl %al, %eax") - 1))
                  return defer_unsupported_asm(p, open, assembly);
                if (accept(p, ":")) {
                  size_t clobber_end = p->pos;
                  if (!inline_asm_clobber_list_is(p, &clobber_end))
                    return defer_unsupported_asm(p, open, assembly);
                  p->pos = clobber_end;
                }
                lower_condition_register_output = true;
              } else if (assembly_len == sizeof("bsfl %1,%0") - 1 &&
                         memcmp(assembly, "bsfl %1,%0", assembly_len) == 0 &&
                         simple_register_output) {
                size_t output_constraint_end = p->pos;
                ny_ctok_t output_name;
                if (!parse_inline_asm_operand_name(p) ||
                    !inline_asm_string_sequence_is(p, "=r",
                                                   &output_constraint_end))
                  return defer_unsupported_asm(p, open, assembly);
                p->pos = output_constraint_end;
                if (!expect(p, "(") || !parse_name(p, &output_name) ||
                    !expect(p, ")") || !expect(p, ":"))
                  return defer_unsupported_asm(p, open, assembly);
                asm_output = find_local(p, output_name);
                size_t input_constraint_end = p->pos;
                if (!asm_output || asm_output->type != C_I32 ||
                    !inline_asm_string_sequence_is(p, "rm",
                                                   &input_constraint_end))
                  return defer_unsupported_asm(p, open, assembly);
                p->pos = input_constraint_end;
                if (!expect(p, "("))
                  return defer_unsupported_asm(p, open, assembly);
                c_value_t input = parse_expr(p, 1);
                if (input.value < 0 || !c_type_is_integer(input.type) ||
                    !expect(p, ")") || !expect(p, ","))
                  return defer_unsupported_asm(p, open, assembly);
                size_t tied_constraint_end = p->pos;
                if (!inline_asm_string_sequence_is(p, "0",
                                                   &tied_constraint_end))
                  return defer_unsupported_asm(p, open, assembly);
                p->pos = tied_constraint_end;
                int64_t tied_value = 0;
                if (!expect(p, "(") ||
                    !parse_global_const_expr(p, &p->pos, &tied_value) ||
                    tied_value != -1 || !expect(p, ")"))
                  return defer_unsupported_asm(p, open, assembly);
                if (accept(p, ":")) {
                  size_t clobber_end = p->pos;
                  if (!inline_asm_clobber_list_is(p, &clobber_end))
                    return defer_unsupported_asm(p, open, assembly);
                  p->pos = clobber_end;
                }
                asm_bsf_input = input.value;
                lower_bsf = true;
              } else if (assembly_len && simple_register_output &&
                         !(assembly_len == sizeof("leaq %c1(%%rip), %0") - 1 &&
                           memcmp(assembly, "leaq %c1(%%rip), %0",
                                  assembly_len) == 0)) {
                size_t output_constraint_end = p->pos;
                if (!parse_inline_asm_operand_name(p) ||
                    !inline_asm_string_sequence_is(p, "=r",
                                                   &output_constraint_end))
                  return defer_unsupported_asm(p, open, assembly);
                p->pos = output_constraint_end;
                ny_ctok_t output_name;
                if (!expect(p, "(") || !parse_name(p, &output_name) ||
                    !expect(p, ")"))
                  return defer_unsupported_asm(p, open, assembly);
                asm_output = find_local(p, output_name);
                if (!asm_output || asm_output->array_rank ||
                    asm_output->is_const ||
                    (!c_type_is_integer(asm_output->type) &&
                     !c_type_is_pointer(asm_output->type)))
                  return defer_unsupported_asm(p, open, assembly);

                if (accept(p, ":")) {
                  if (!accept(p, ":") || tok_is(p, p->pos, ")"))
                    return defer_unsupported_asm(p, open, assembly);
                  size_t clobber_end = p->pos;
                  if (!inline_asm_clobber_list_is(p, &clobber_end))
                    return defer_unsupported_asm(p, open, assembly);
                  p->pos = clobber_end;
                }
                immediate_values[0] =
                    assembly_len >= sizeof("movl ") - 1 &&
                            memcmp(assembly, "movl ", sizeof("movl ") - 1) == 0
                        ? "%eax"
                        : "%rax";
                if (!substitute_inline_asm_immediate(
                        &assembly, &assembly_len, &assembly_cap,
                        immediate_values, 1, SIZE_MAX))
                  return defer_unsupported_asm(p, open, assembly);
                lower_named_register_output = true;
              } else if (!assembly_len) {
                size_t output_constraint_end = p->pos;
                ny_ctok_t output_name;
                size_t input_constraint_end;
                if (!parse_inline_asm_operand_name(p) ||
                    !inline_asm_string_sequence_is(p, "=r",
                                                   &output_constraint_end)) {
                  return defer_unsupported_asm(p, open, assembly);
                }
                p->pos = output_constraint_end;
                if (!expect(p, "(") || !parse_name(p, &output_name) ||
                    !expect(p, ")") || !expect(p, ":")) {
                  return defer_unsupported_asm(p, open, assembly);
                }
                input_constraint_end = p->pos;
                if (!inline_asm_string_sequence_is(p, "0",
                                                   &input_constraint_end)) {
                  return defer_unsupported_asm(p, open, assembly);
                }
                p->pos = input_constraint_end;
                if (!expect(p, "(")) {
                  return defer_unsupported_asm(p, open, assembly);
                }
                c_value_t input = parse_expr(p, 1);
                if (input.value < 0 || !expect(p, ")")) {
                  free(assembly);
                  return false;
                }
                asm_output = find_local(p, output_name);
                if (accept(p, ":")) {
                  return defer_unsupported_asm(p, open, assembly);
                }
                if (!asm_output || asm_output->array_rank ||
                    asm_output->is_const ||
                    !(c_type_is_pointer(asm_output->type) ||
                      c_type_is_integer(asm_output->type)) ||
                    !(c_type_is_pointer(input.type) ||
                      c_type_is_integer(input.type))) {
                  return defer_unsupported_asm(p, open, assembly);
                }
                lower_tied_register_copy = true;
                tied_input_value = input.value;
              } else if (assembly_len == sizeof("leaq %c1(%%rip), %0") - 1 &&
                         memcmp(assembly, "leaq %c1(%%rip), %0",
                                assembly_len) == 0) {
                size_t output_constraint_end = p->pos;
                ny_ctok_t output_name;
                size_t input_constraint_end = p->pos;
                if (!parse_inline_asm_operand_name(p) ||
                    !inline_asm_string_sequence_is(p, "=r",
                                                   &output_constraint_end)) {
                  return defer_unsupported_asm(p, open, assembly);
                }
                p->pos = output_constraint_end;
                if (!expect(p, "(") || !parse_name(p, &output_name) ||
                    !expect(p, ")") || !expect(p, ":")) {
                  return defer_unsupported_asm(p, open, assembly);
                }
                if (!parse_inline_asm_operand_name(p) ||
                    !inline_asm_string_sequence_is(p, "i",
                                                   &input_constraint_end)) {
                  return defer_unsupported_asm(p, open, assembly);
                }
                p->pos = input_constraint_end;
                ny_ctok_t input_name;
                if (!expect(p, "(") || !parse_name(p, &input_name) ||
                    !expect(p, ")")) {
                  return defer_unsupported_asm(p, open, assembly);
                }
                asm_output = find_local(p, output_name);
                asm_input = find_local(p, input_name);
                if (accept(p, ":")) {
                  return defer_unsupported_asm(p, open, assembly);
                }
                if (!asm_output || !asm_input || asm_output->array_rank ||
                    asm_input->array_rank || asm_output->is_const ||
                    !c_type_is_pointer(asm_output->type) ||
                    !c_type_is_pointer(asm_input->type) ||
                    !c_pointer_compatible(asm_output->type, asm_input->type)) {
                  return defer_unsupported_asm(p, open, assembly);
                }
                lower_rip_relative_pointer = true;
              } else {
                size_t output_start = p->pos;
                size_t output_constraint_end = p->pos;
                ny_ctok_t output_name;
                bool has_output_name = parse_inline_asm_operand_name(p);
                bool is_memory_output =
                    has_output_name && inline_asm_string_sequence_is(
                                           p, "=rm", &output_constraint_end);
                p->pos = output_start;
                if (is_memory_output) {
                  if (!parse_inline_asm_operand_name(p)) {
                    return defer_unsupported_asm(p, open, assembly);
                  }
                  p->pos = output_constraint_end;
                  if (!expect(p, "(") || !parse_name(p, &output_name) ||
                      !expect(p, ")")) {
                    return defer_unsupported_asm(p, open, assembly);
                  }
                  c_local_t *local = find_local(p, output_name);
                  size_t placeholder_count = 0;
                  bool valid_template = true;
                  for (size_t i = 0; i < assembly_len; ++i) {
                    if (assembly[i] != '%')
                      continue;
                    if (i + 1 >= assembly_len || assembly[i + 1] != '0') {
                      valid_template = false;
                      break;
                    }
                    ++placeholder_count;
                    ++i;
                  }
                  if (accept(p, ":")) {
                    if (!accept(p, ":"))
                      return defer_unsupported_asm(p, open, assembly);
                    size_t clobber_end = p->pos;
                    if (!tok_is(p, p->pos, ")") &&
                        !inline_asm_clobber_list_matches(p, &clobber_end, true))
                      return defer_unsupported_asm(p, open, assembly);
                    p->pos = clobber_end;
                  }
                  if (!local || local->array_rank || local->is_const ||
                      (!(local->type == C_I64 || local->type == C_U64 ||
                         (p->abi == NYC_ABI_I386 &&
                          (local->type == C_I32 || local->type == C_U32))) &&
                       !c_type_is_pointer(local->type)) ||
                      !valid_template || placeholder_count != 1) {
                    return defer_unsupported_asm(p, open, assembly);
                  }
                  local->has_integer_constant = false;
                  local->address_taken = true;
                  asm_memory_output_value = emit_value(
                      p, (nyr_inst_t){.op = NYIR_ADDR_LOCAL,
                                      .dst = -1,
                                      .imm = (int64_t)local->slot,
                                      .scalar_class = NYIR_SCALAR_I64_RAW});
                  immediate_values[0] =
                      p->abi == NYC_ABI_I386 ? "(%eax)" : "(%rax)";
                  if (asm_memory_output_value < 0 ||
                      !substitute_inline_asm_immediate(
                          &assembly, &assembly_len, &assembly_cap,
                          immediate_values, 1, SIZE_MAX))
                    return defer_unsupported_asm(p, open, assembly);
                  lower_named_memory_output = true;
                } else {
                  p->pos = output_start;
                  output_constraint_end = p->pos;
                  if (!parse_inline_asm_operand_name(p) ||
                      !inline_asm_string_sequence_is(p, "+r",
                                                     &output_constraint_end)) {
                    return defer_unsupported_asm(p, open, assembly);
                  }
                  p->pos = output_constraint_end;
                  if (!expect(p, "(") || !parse_name(p, &output_name) ||
                      !expect(p, ")")) {
                    return defer_unsupported_asm(p, open, assembly);
                  }
                  c_global_t *output_global = find_global(p, output_name);
                  if (!output_global ||
                      !global_register_is_x86_rsp(output_global) ||
                      (!c_type_is_integer(output_global->type) &&
                       !c_type_is_pointer(output_global->type)) ||
                      assembly_len <= sizeof("call ") - 1 ||
                      memcmp(assembly, "call ", sizeof("call ") - 1) != 0 ||
                      memchr(assembly, '%', assembly_len) ||
                      memchr(assembly, '\n', assembly_len) ||
                      memchr(assembly, ';', assembly_len) || accept(p, ":")) {
                    return defer_unsupported_asm(p, open, assembly);
                  }
                  /*
                   * This operand models the stack pointer around a direct call.
                   * NYC keeps C values out of RSP, and the call ABI restores
                   * its value.
                   */
                }
              }
            }
          }
        }
      }
    }
  }

finish_inline_asm_operands:
  if (!expect(p, ")") || !expect(p, ";")) {
    free(assembly);
    return false;
  }
  if (lower_x86_register_snapshot) {
    static const int64_t modes[] = {-150, 0, -151};
    for (size_t output_index = 0; output_index < 3; ++output_index) {
      int value =
          output_index == 1
              ? emit_value(p, (nyr_inst_t){.op = NYIR_READ_X86_RSP,
                                           .dst = -1,
                                           .scalar_class = NYIR_SCALAR_I64_RAW})
              : emit_value(p, (nyr_inst_t){.op = NYIR_INLINE_ASM,
                                           .dst = -1,
                                           .a = -1,
                                           .b = -1,
                                           .c = -1,
                                           .d = -1,
                                           .e = -1,
                                           .f = -1,
                                           .imm = modes[output_index],
                                           .effects = NYIR_EFFECT_VOLATILE,
                                           .scalar_class = NYIR_SCALAR_I64_RAW,
                                           .symbol = ""});
      if (value < 0 ||
          !lower_store_field(p, x86_snapshot_outputs[output_index].address,
                             value, false,
                             x86_snapshot_outputs[output_index].is_volatile)) {
        free(assembly);
        return false;
      }
    }
    free(assembly);
    return true;
  }
  if (lower_empty_memory_barrier) {
    free(assembly);
    return true;
  }
  if (lower_far_jump) {
    bool ok = emit(p,
                   (nyr_inst_t){.op = NYIR_INLINE_ASM,
                                .dst = -1,
                                .a = asm_far_jump_memory,
                                .b = asm_far_jump_type,
                                .imm = -146,
                                .effects = NYIR_EFFECT_READ_MEMORY |
                                           NYIR_EFFECT_MAY_TRAP |
                                           NYIR_EFFECT_VOLATILE,
                                .symbol = (const char *)assembly},
                   NULL);
    free(assembly);
    return ok;
  }
  if (lower_indirect_jump) {
    bool ok = emit(
        p,
        (nyr_inst_t){.op = NYIR_INLINE_ASM,
                     .dst = -1,
                     .a = asm_indirect_jump_target,
                     .b = asm_indirect_jump_argument,
                     .imm = -157,
                     .effects = NYIR_EFFECT_READ_MEMORY |
                                NYIR_EFFECT_WRITE_MEMORY | NYIR_EFFECT_CONTROL |
                                NYIR_EFFECT_VOLATILE | NYIR_EFFECT_MAY_TRAP,
                     .symbol = (const char *)assembly},
        NULL);
    free(assembly);
    return ok;
  }
  if (lower_segment_load_asm) {
    bool ok = emit(p,
                   (nyr_inst_t){.op = NYIR_INLINE_ASM,
                                .dst = -1,
                                .a = asm_segment_load_value,
                                .imm = -148,
                                .effects = NYIR_EFFECT_VOLATILE,
                                .symbol = (const char *)assembly},
                   NULL);
    free(assembly);
    return ok;
  }
  if (lower_apic_alt_io) {
    int output =
        emit_value(p, (nyr_inst_t){.op = NYIR_INLINE_ASM,
                                   .dst = -1,
                                   .a = asm_alt_io_input,
                                   .b = asm_alt_io_memory,
                                   .imm = -147,
                                   .effects = NYIR_EFFECT_READ_MEMORY |
                                              NYIR_EFFECT_WRITE_MEMORY |
                                              NYIR_EFFECT_VOLATILE,
                                   .symbol = (const char *)assembly});
    bool ok = output >= 0 && lower_local_store(p, asm_alt_io_output, output);
    free(assembly);
    return ok;
  }
  if (asm_debug_input >= 0) {
    static const char *const debug_regs[] = {"db0", "db1", "db2", "db3",
                                             "db4", "db5", "db6", "db7"};
    bool ok = asm_debug_reg >= 0 && asm_debug_reg < 8 &&
              emit(p,
                   (nyr_inst_t){.op = NYIR_INLINE_ASM,
                                .dst = -1,
                                .a = asm_debug_input,
                                .imm = -32,
                                .effects = NYIR_EFFECT_VOLATILE,
                                .symbol = debug_regs[asm_debug_reg]},
                   NULL);
    free(assembly);
    return ok;
  }
  if (lower_tied_register_copy) {
    bool ok = lower_local_store(p, asm_output, tied_input_value);
    free(assembly);
    return ok;
  }
  if (lower_runtime_const_ptr) {
    int output = emit_value(p, (nyr_inst_t){.op = NYIR_INLINE_ASM,
                                            .dst = -1,
                                            .a = -1,
                                            .b = -1,
                                            .c = -1,
                                            .d = -1,
                                            .imm = -1,
                                            .effects = NYIR_EFFECT_VOLATILE,
                                            .symbol = (const char *)assembly});
    bool ok = output >= 0 && lower_local_store(p, asm_output, output);
    free(assembly);
    return ok;
  }
  if (lower_runtime_const_shift) {
    int input = lower_local_load(p, asm_output);
    int output =
        input >= 0
            ? emit_value(p, (nyr_inst_t){.op = NYIR_INLINE_ASM,
                                         .dst = -1,
                                         .a = input,
                                         .imm = -149,
                                         .effects = NYIR_EFFECT_VOLATILE,
                                         .symbol = runtime_shift_section})
            : -1;
    bool ok = output >= 0 && lower_local_store(p, asm_output, output);
    free(assembly);
    return ok;
  }
  if (lower_runtime_ptr_clamp) {
    int output = emit_value(p, (nyr_inst_t){.op = NYIR_INLINE_ASM,
                                            .dst = -1,
                                            .a = runtime_ptr_clamp_bound,
                                            .b = runtime_ptr_clamp_initial,
                                            .imm = -156,
                                            .symbol = "runtime_ptr_clamp",
                                            .effects = NYIR_EFFECT_VOLATILE});
    bool ok = output >= 0 && lower_local_store(p, asm_output, output);
    free(assembly);
    return ok;
  }
  if (lower_tzcnt) {
    nyr_inst_t inst = {.op = NYIR_INLINE_ASM,
                       .dst = -1,
                       .a = asm_tzcnt_input,
                       .imm = -13,
                       .symbol = "tzcntq %rax, %rax"};
    int output = emit_value(p, inst);
    bool ok = output >= 0 && lower_local_store(p, asm_output, output);
    free(assembly);
    return ok;
  }
  if (lower_bsr) {
    int initial = lower_local_load(p, asm_output);
    int output =
        initial < 0
            ? -1
            : emit_value(p, (nyr_inst_t){.op = NYIR_INLINE_ASM,
                                         .dst = -1,
                                         .a = asm_bsr_input,
                                         .b = initial,
                                         .imm = -25,
                                         .effects = NYIR_EFFECT_VOLATILE,
                                         .scalar_class = NYIR_SCALAR_I64_RAW,
                                         .symbol = "bsrq"});
    bool ok = output >= 0 && lower_local_store(p, asm_output, output);
    free(assembly);
    return ok;
  }
  if (lower_bsf) {
    int input = normalize_u32(p, asm_bsf_input);
    c_value_t trailing =
        input >= 0 ? lower_ctz_value(p, input) : (c_value_t){-1, C_BAD, NULL};
    int zero = emit_i64_constant(p, 0);
    int minus_one = emit_i64_constant(p, -1);
    int is_zero = input >= 0 && zero >= 0
                      ? emit_i64_compare(p, input, zero, NYIR_CMP_EQ)
                      : -1;
    int result = is_zero >= 0 && minus_one >= 0 && trailing.value >= 0
                     ? emit_i64_select(p, is_zero, minus_one, trailing.value)
                     : -1;
    bool ok = result >= 0 && lower_local_store(p, asm_output, result);
    free(assembly);
    return ok;
  }
  if (lower_atomic_inc || lower_atomic_dec) {
    nyr_inst_t inst = {.op = NYIR_INLINE_ASM,
                       .dst = -1,
                       .a = asm_memory_output_value,
                       .imm = lower_atomic_dec ? -16 : -15,
                       .symbol = lower_atomic_dec ? "lock decq (%rax)"
                                                  : "lock incq (%rax)"};
    bool ok = emit(p, inst, NULL);
    free(assembly);
    return ok;
  }
  if (lower_atomic_arithmetic_condition) {
    int result =
        emit_value(p, (nyr_inst_t){.op = NYIR_INLINE_ASM,
                                   .dst = -1,
                                   .a = asm_memory_output_value,
                                   .b = asm_memory_register_input_value,
                                   .c = -1,
                                   .imm = -81,
                                   .effects = NYIR_EFFECT_READ_MEMORY |
                                              NYIR_EFFECT_WRITE_MEMORY |
                                              NYIR_EFFECT_VOLATILE,
                                   .scalar_class = NYIR_SCALAR_I64_RAW,
                                   .symbol = atomic_arithmetic_mnemonic});
    bool ok = result >= 0 && lower_local_store(p, asm_condition_output, result);
    free(assembly);
    return ok;
  }
  if (lower_atomic_bitwise_condition) {
    int result =
        emit_value(p, (nyr_inst_t){.op = NYIR_INLINE_ASM,
                                   .dst = -1,
                                   .a = asm_memory_output_value,
                                   .b = asm_memory_register_input_value,
                                   .imm = -47,
                                   .effects = NYIR_EFFECT_READ_MEMORY |
                                              NYIR_EFFECT_WRITE_MEMORY |
                                              NYIR_EFFECT_VOLATILE,
                                   .symbol = "xorb"});
    bool ok = result >= 0 && lower_local_store(p, asm_condition_output, result);
    free(assembly);
    return ok;
  }
  if (lower_bt_condition) {
    int result = emit_value(
        p, (nyr_inst_t){
               .op = NYIR_INLINE_ASM,
               .dst = -1,
               .a = asm_memory_output_value,
               .b = asm_memory_register_input_value,
               .imm = -47,
               .effects = NYIR_EFFECT_READ_MEMORY | NYIR_EFFECT_VOLATILE,
               .symbol = bt_condition_symbol ? bt_condition_symbol : "btq"});
    bool ok = result >= 0 && lower_local_store(p, asm_condition_output, result);
    free(assembly);
    return ok;
  }
  if (lower_atomic_bts_condition) {
    int result =
        emit_value(p, (nyr_inst_t){.op = NYIR_INLINE_ASM,
                                   .dst = -1,
                                   .a = asm_memory_output_value,
                                   .b = asm_memory_register_input_value,
                                   .imm = -47,
                                   .effects = NYIR_EFFECT_READ_MEMORY |
                                              NYIR_EFFECT_WRITE_MEMORY |
                                              NYIR_EFFECT_VOLATILE,
                                   .symbol = "btsl"});
    bool ok = result >= 0 && lower_local_store(p, asm_condition_output, result);
    free(assembly);
    return ok;
  }
  if (lower_atomic_memory_rmw) {
    bool ok = emit(
        p,
        (nyr_inst_t){
            .op = NYIR_INLINE_ASM,
            .dst = -1,
            .a = asm_memory_output_value,
            .b = asm_memory_register_input_value,
            .imm = -33,
            .flags = atomic_memory_rmw_locked ? 0 : NYIR_INST_F_ASM_UNLOCKED,
            .effects = NYIR_EFFECT_READ_MEMORY | NYIR_EFFECT_WRITE_MEMORY |
                       NYIR_EFFECT_VOLATILE,
            .symbol = atomic_memory_rmw_mnemonic},
        NULL);
    free(assembly);
    return ok;
  }
  if (lower_atomic_memory_rmw64) {
    bool ok = emit(p,
                   (nyr_inst_t){.op = NYIR_INLINE_ASM,
                                .dst = -1,
                                .a = asm_memory_output_value,
                                .b = asm_memory_register_input_value,
                                .imm = -77,
                                .effects = NYIR_EFFECT_READ_MEMORY |
                                           NYIR_EFFECT_WRITE_MEMORY |
                                           NYIR_EFFECT_VOLATILE,
                                .symbol = atomic_memory_rmw_mnemonic},
                   NULL);
    free(assembly);
    return ok;
  }
  if (lower_rip_relative_pointer) {
    int input_value = lower_local_load(p, asm_input);
    int result =
        input_value < 0
            ? -1
            : emit_value(p, (nyr_inst_t){.op = NYIR_INLINE_ASM,
                                         .dst = -1,
                                         .a = input_value,
                                         .imm = -196,
                                         .scalar_class = NYIR_SCALAR_I64_RAW});
    bool ok = result >= 0 && lower_local_store(p, asm_output, result);
    free(assembly);
    return ok;
  }
  if (!append_asm_bytes(&assembly, &assembly_len, &assembly_cap, "\0", 1)) {
    free(assembly);
    return false;
  }
  nyr_inst_t inst = {.op = NYIR_INLINE_ASM,
                     .dst = -1,
                     .a = asm_memory_input_value,
                     .d = -1,
                     .symbol = (const char *)assembly};
  if (lower_named_memory_output) {
    inst.a = asm_memory_output_value;
    inst.b = asm_memory_register_input_value;
    if (lower_named_memory_condition_output) {
      inst.imm = asm_memory_register_input_value >= 0 ? -8 : -4;
      int result = emit_value(p, inst);
      bool ok =
          result >= 0 && lower_local_store(p, asm_condition_output, result);
      free(assembly);
      return ok;
    }
    inst.imm = asm_memory_register_input_value >= 0 ? -3 : -2;
    bool ok = emit(p, inst, NULL);
    free(assembly);
    return ok;
  }
  if (lower_memory_register_output) {
    inst.a = asm_memory_output_value;
    inst.b = asm_memory_register_input_value;
    inst.imm = asm_memory_register_imm;
    if (inst.imm == -50) {
      inst.c = -1;
      bool is_unsigned =
          asm_output &&
          (asm_output->type == C_U8 || asm_output->type == C_U16 ||
           asm_output->type == C_U32 || asm_output->type == C_U64);
      inst.symbol = (asm_output->type == C_CHAR || asm_output->type == C_U8)
                        ? (is_unsigned ? "xaddbu" : "xaddbs")
                    : (asm_output->type == C_I16 || asm_output->type == C_U16)
                        ? (is_unsigned ? "xaddwu" : "xaddws")
                    : (asm_output->type == C_I32 || asm_output->type == C_U32)
                        ? (is_unsigned ? "xaddlu" : "xaddls")
                        : (asm_memory_register_xadd_unlocked ? "unlocked:xaddq"
                                                             : "xaddq");
      if (asm_memory_register_xadd_gs) {
        const char *base_symbol = inst.symbol;
        if (strcmp(base_symbol, "xaddbu") == 0)
          inst.symbol = "xaddbu-gs";
        else if (strcmp(base_symbol, "xaddbs") == 0)
          inst.symbol = "xaddbs-gs";
        else if (strcmp(base_symbol, "xaddwu") == 0)
          inst.symbol = "xaddwu-gs";
        else if (strcmp(base_symbol, "xaddws") == 0)
          inst.symbol = "xaddws-gs";
        else if (strcmp(base_symbol, "xaddlu") == 0)
          inst.symbol = "xaddlu-gs";
        else if (strcmp(base_symbol, "xaddls") == 0)
          inst.symbol = "xaddls-gs";
        else if (strcmp(base_symbol, "xaddq") == 0)
          inst.symbol = "unlocked:gs:xaddq";
      }
    }
    int output = emit_value(p, inst);
    bool ok = output >= 0 && lower_local_store(p, asm_output, output);
    free(assembly);
    return ok;
  }
  if (lower_cmpxchg_value_output) {
    int output = emit_value(
        p, (nyr_inst_t){.op = NYIR_INLINE_ASM,
                        .dst = -1,
                        .a = asm_memory_output_value,
                        .b = asm_cmpxchg_expected_value,
                        .c = asm_cmpxchg_new_value,
                        .d = -1,
                        .e = -1,
                        .f = -1,
                        .imm = lower_cmpxchg64             ? -16
                               : lower_cmpxchg8            ? -155
                               : lower_cmpxchg16           ? -145
                               : asm_output->type == C_I32 ? -11
                                                           : -12,
                        .effects = NYIR_EFFECT_READ_MEMORY |
                                   NYIR_EFFECT_WRITE_MEMORY |
                                   NYIR_EFFECT_MAY_TRAP | NYIR_EFFECT_VOLATILE,
                        .symbol = (const char *)assembly});
    bool ok = output >= 0 && lower_local_store(p, asm_output, output);
    free(assembly);
    return ok;
  }
  if (lower_cmpxchg_outputs) {
    inst.a = asm_memory_output_value;
    inst.b = asm_memory_register_input_value;
    inst.c = asm_cmpxchg_new_value;
    inst.imm = lower_cmpxchg64 ? -16
               : lower_cmpxchg8 || asm_output->type == C_U8 ||
                       asm_output->type == C_CHAR
                   ? -155
               : lower_cmpxchg16           ? -145
               : asm_output->type == C_U32 ? -12
                                           : -11;
    int output = emit_value(p, inst);
    int success =
        output >= 0
            ? emit_i64_compare(p, output, asm_memory_register_input_value,
                               NYIR_CMP_EQ)
            : -1;
    bool ok = success >= 0 && lower_local_store(p, asm_output, output) &&
              lower_local_store(p, asm_condition_output, success);
    free(assembly);
    return ok;
  }

  if (lower_named_register_output) {
    unescape_inline_asm_percents(assembly, &assembly_len);
    if (lower_named_register_memory_input) {
      inst.a = asm_memory_input_value;
      inst.imm = -9;
    } else {
      inst.imm = -1;
    }
    int output = emit_value(p, inst);
    bool ok = output >= 0 && lower_local_store(p, asm_output, output);
    free(assembly);
    return ok;
  }
  if (lower_condition_register_output) {
    unescape_inline_asm_percents(assembly, &assembly_len);
    inst.b = asm_memory_register_input_value;
    inst.imm = -7;
    int output = emit_value(p, inst);
    bool ok = output >= 0 && lower_local_store(p, asm_condition_output, output);
    free(assembly);
    return ok;
  }
  if (lower_invpcid) {
    inst.a = asm_memory_input_value;
    inst.b = asm_memory_register_input_value;
    inst.imm = -154;
    inst.effects = NYIR_EFFECT_READ_MEMORY | NYIR_EFFECT_WRITE_MEMORY |
                   NYIR_EFFECT_VOLATILE;
    inst.symbol = "invpcid";
    bool ok = emit(p, inst, NULL);
    free(assembly);
    return ok;
  }
  unescape_inline_asm_percents(assembly, &assembly_len);
  if (lower_fixed_register_input) {
    inst.a = asm_fixed_register_inputs[0];
    inst.b = asm_fixed_register_inputs[1];
    inst.c = asm_fixed_register_inputs[2];
    inst.imm = -5;
  } else if (asm_memory_register_input_value >= 0) {
    inst.b = asm_memory_register_input_value;
    inst.imm = -6;
  }
  bool ok = emit(p, inst, NULL);
  free(assembly);
  return ok;
}

#undef INLINE_ASM_HAS
