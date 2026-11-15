/* C string literal lowering, included by parse.c. */
static bool decode_adjacent_string_tokens(c_parser_t *p,
                                          unsigned char **string_out,
                                          size_t *string_len_out) {
  if (!p || !string_out || !string_len_out || p->pos >= p->token_count ||
      p->tokens[p->pos].kind != NY_CTOK_STRING)
    return false;
  *string_out = NULL;
  *string_len_out = 0;
  unsigned char *string = NULL;
  size_t string_len = 0;
  while (p->pos < p->token_count && p->tokens[p->pos].kind == NY_CTOK_STRING) {
    unsigned char *part = NULL;
    size_t part_len = 0;
    if (!decode_string_token(p->tokens[p->pos++], &part, &part_len)) {
      free(part);
      free(string);
      fail(p, "invalid C string literal");
      return false;
    }
    if (!part_len) {
      free(part);
      free(string);
      fail(p, "invalid C string literal");
      return false;
    }
    size_t old_content = string_len ? string_len - 1 : 0;
    size_t part_content = part_len - 1;
    if (part_content > SIZE_MAX - old_content - 1) {
      free(part);
      free(string);
      fail(p, "C string literal exceeds frontend limits");
      return false;
    }
    unsigned char *joined = realloc(string, old_content + part_content + 1);
    if (!joined) {
      free(part);
      free(string);
      fail(p, "out of memory concatenating C string literal");
      return false;
    }
    string = joined;
    memcpy(string + old_content, part, part_content + 1);
    string_len = old_content + part_content + 1;
    free(part);
  }
  *string_out = string;
  *string_len_out = string_len;
  return true;
}

static c_value_t lower_string_literal(c_parser_t *p) {
  unsigned char *string = NULL;
  size_t string_len = 0;
  if (!decode_adjacent_string_tokens(p, &string, &string_len)) {
    free(string);
    if (!p->err || !p->err_len || !p->err[0])
      fail(p, "invalid C string literal");
    return (c_value_t){-1, C_BAD, NULL};
  }
  if (p->global_count >= p->global_capacity) {
    free(string);
    fail(p, "C frontend global data table capacity exceeded");
    return (c_value_t){-1, C_BAD, NULL};
  }
  char generated_name[48];
  int written = snprintf(generated_name, sizeof(generated_name),
                         "__nyc_string_%zu", p->global_count);
  if (written < 0 || (size_t)written >= sizeof(generated_name)) {
    free(string);
    fail(p, "generated C string symbol exceeds frontend limit");
    return (c_value_t){-1, C_BAD, NULL};
  }
  char *owned_name = malloc((size_t)written + 1);
  if (!owned_name) {
    free(string);
    fail(p, "out of memory naming C string literal");
    return (c_value_t){-1, C_BAD, NULL};
  }
  memcpy(owned_name, generated_name, (size_t)written + 1);
  ny_ctok_t name = {
      .kind = NY_CTOK_IDENT, .start = owned_name, .len = (size_t)written};
  p->globals[p->global_count++] = (c_global_t){.name = name,
                                               .bytes = string,
                                               .owned_name = owned_name,
                                               .len = string_len,
                                               .type = C_CHAR_PTR,
                                               .is_array = true,
                                               .is_const = true};
  char symbol[64];
  int symbol_len =
      snprintf(symbol, sizeof(symbol), "__nyc_data_%s", generated_name);
  if (symbol_len < 0 || (size_t)symbol_len >= sizeof(symbol)) {
    fail(p, "generated C string data symbol exceeds frontend limit");
    return (c_value_t){-1, C_BAD, NULL};
  }
  char *owned_symbol = malloc((size_t)symbol_len + 1);
  if (!owned_symbol) {
    fail(p, "out of memory lowering C string literal");
    return (c_value_t){-1, C_BAD, NULL};
  }
  memcpy(owned_symbol, symbol, (size_t)symbol_len + 1);
  int address = emit_value(
      p,
      (nyr_inst_t){.op = NYIR_ADDR_SYMBOL, .dst = -1, .symbol = owned_symbol});
  free(owned_symbol);
  return address < 0 ? (c_value_t){-1, C_BAD, NULL}
                     : (c_value_t){address, C_CHAR_PTR, NULL};
}
