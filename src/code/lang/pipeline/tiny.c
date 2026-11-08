/*
 * Tiny compiler pipeline: a minimal, self-contained compilation path
 * used for REPL evaluations, inline -c execution, and quick smoke tests.
 */
static bool ny_trace_compile_enabled(void) {
  return ny_env_enabled("NYTRIX_TRACE_COMPILE") ||
         ny_env_enabled("NYTRIX_TRACE_CODEGEN");
}

static void ny_collect_ir_stats(LLVMModuleRef module, ny_ir_stats_t *out) {
  if (!out)
    return;
  memset(out, 0, sizeof(*out));
  if (!module)
    return;
  for (LLVMValueRef fn = LLVMGetFirstFunction(module); fn;
       fn = LLVMGetNextFunction(fn)) {
    if (LLVMCountBasicBlocks(fn) == 0)
      continue;
    out->funcs++;
    for (LLVMBasicBlockRef bb = LLVMGetFirstBasicBlock(fn); bb;
         bb = LLVMGetNextBasicBlock(bb)) {
      out->blocks++;
      for (LLVMValueRef inst = LLVMGetFirstInstruction(bb); inst;
           inst = LLVMGetNextInstruction(inst)) {
        out->insts++;
        LLVMOpcode op = LLVMGetInstructionOpcode(inst);
        if (op == LLVMAlloca)
          out->allocas++;
        else if (op == LLVMPHI)
          out->phis++;
      }
    }
  }
}

static void ny_trace_ir_stats(const char *phase, LLVMModuleRef module) {
  if (!ny_trace_compile_enabled())
    return;
  ny_ir_stats_t st = {0};
  ny_collect_ir_stats(module, &st);
  fprintf(stderr,
          "TRACE_COMPILE %s funcs=%" PRIu64 " blocks=%" PRIu64 " insts=%" PRIu64
          " allocas=%" PRIu64 " phis=%" PRIu64 "\n",
          phase ? phase : "ir", st.funcs, st.blocks, st.insts, st.allocas,
          st.phis);
}

static void ny_trace_file_size(const char *label, const char *path) {
  if (!ny_trace_compile_enabled() || !path || !*path)
    return;
  struct stat st;
  if (stat(path, &st) != 0)
    return;
  fprintf(stderr, "TRACE_COMPILE %s path=%s size=%lld bytes\n",
          label ? label : "artifact", path, (long long)st.st_size);
}

static const char *ny_skip_ws(const char *s) {
  while (s && *s && isspace((unsigned char)*s))
    s++;
  return s;
}

static bool ny_tiny_parse_i64_add(const char **cursor, int64_t *out,
                                  unsigned depth);
static bool ny_tiny_parse_i64_unary(const char **cursor, int64_t *out,
                                    unsigned depth);

static bool ny_tiny_parse_i64_primary(const char **cursor, int64_t *out,
                                      unsigned depth) {
  if (!cursor || !*cursor || !out || depth > 64)
    return false;
  const char *p = ny_skip_ws(*cursor);
  if (*p == '(') {
    ++p;
    if (!ny_tiny_parse_i64_add(&p, out, depth + 1))
      return false;
    p = ny_skip_ws(p);
    if (*p != ')')
      return false;
    *cursor = p + 1;
    return true;
  }
  if (!isdigit((unsigned char)*p))
    return false;
  uint64_t value = 0;
  do {
    unsigned digit = (unsigned)(*p - '0');
    if (value > (UINT64_MAX - digit) / 10)
      return false;
    value = value * 10 + digit;
    ++p;
  } while (isdigit((unsigned char)*p));
  if (value > INT64_MAX)
    return false;
  *out = (int64_t)value;
  *cursor = p;
  return true;
}

static bool ny_tiny_parse_i64_unary(const char **cursor, int64_t *out,
                                    unsigned depth) {
  if (!cursor || !*cursor || !out || depth > 64)
    return false;
  const char *p = ny_skip_ws(*cursor);
  if (*p == '+' || *p == '-') {
    char sign = *p++;
    int64_t value = 0;
    if (!ny_tiny_parse_i64_unary(&p, &value, depth + 1) ||
        (sign == '-' && value == INT64_MIN))
      return false;
    *out = sign == '-' ? -value : value;
    *cursor = p;
    return true;
  }
  if (!ny_tiny_parse_i64_primary(&p, out, depth))
    return false;
  *cursor = p;
  return true;
}

static bool ny_tiny_i64_add(int64_t a, int64_t b, int64_t *out) {
  if ((b > 0 && a > INT64_MAX - b) || (b < 0 && a < INT64_MIN - b))
    return false;
  *out = a + b;
  return true;
}

static bool ny_tiny_i64_sub(int64_t a, int64_t b, int64_t *out) {
  if ((b < 0 && a > INT64_MAX + b) || (b > 0 && a < INT64_MIN + b))
    return false;
  *out = a - b;
  return true;
}

static bool ny_tiny_i64_mul(int64_t a, int64_t b, int64_t *out) {
  if (a == 0 || b == 0) {
    *out = 0;
    return true;
  }
  if ((a == -1 && b == INT64_MIN) || (b == -1 && a == INT64_MIN))
    return false;
  if (a > 0) {
    if ((b > 0 && a > INT64_MAX / b) || (b < 0 && b < INT64_MIN / a))
      return false;
  } else if ((b > 0 && a < INT64_MIN / b) || (b < 0 && a < INT64_MAX / b)) {
    return false;
  }
  *out = a * b;
  return true;
}

static bool ny_tiny_parse_i64_mul(const char **cursor, int64_t *out,
                                  unsigned depth) {
  if (!cursor || !*cursor || !out || depth > 64)
    return false;
  const char *p = *cursor;
  int64_t value = 0;
  if (!ny_tiny_parse_i64_unary(&p, &value, depth + 1))
    return false;
  for (;;) {
    p = ny_skip_ws(p);
    char op = *p;
    if (op != '*' && op != '/' && op != '%')
      break;
    ++p;
    int64_t rhs = 0;
    if (!ny_tiny_parse_i64_unary(&p, &rhs, depth + 1))
      return false;
    if (op == '*') {
      if (!ny_tiny_i64_mul(value, rhs, &value))
        return false;
    } else {
      if (rhs == 0 || (value == INT64_MIN && rhs == -1))
        return false;
      value = op == '/' ? value / rhs : value % rhs;
    }
  }
  *cursor = p;
  *out = value;
  return true;
}

static bool ny_tiny_parse_i64_add(const char **cursor, int64_t *out,
                                  unsigned depth) {
  if (!cursor || !*cursor || !out || depth > 64)
    return false;
  const char *p = *cursor;
  int64_t value = 0;
  if (!ny_tiny_parse_i64_mul(&p, &value, depth + 1))
    return false;
  for (;;) {
    p = ny_skip_ws(p);
    char op = *p;
    if (op != '+' && op != '-')
      break;
    ++p;
    int64_t rhs = 0;
    if (!ny_tiny_parse_i64_mul(&p, &rhs, depth + 1) ||
        !(op == '+' ? ny_tiny_i64_add(value, rhs, &value)
                    : ny_tiny_i64_sub(value, rhs, &value)))
      return false;
  }
  *cursor = p;
  *out = value;
  return true;
}

static bool ny_tiny_parse_i64_constant(const char *src, const char **end_out,
                                       char **out_text) {
  if (!src || !end_out || !out_text)
    return false;
  const char *end = src;
  int64_t value = 0;
  if (!ny_tiny_parse_i64_add(&end, &value, 0))
    return false;
  char text[32];
  int len = snprintf(text, sizeof(text), "%" PRId64, value);
  if (len < 0 || (size_t)len >= sizeof(text))
    return false;
  char *copy = ny_strdup(text);
  if (!copy)
    return false;
  *end_out = end;
  *out_text = copy;
  return true;
}

static bool ny_decode_tiny_string_literal(const char *src, const char **end_out,
                                          char **out_text) {
  if (!src || !(*src == '"' || *src == '\'') || !end_out || !out_text)
    return false;
  char quote = *src++;
  size_t src_len = strlen(src);
  if (src_len > SIZE_MAX - 1)
    return false;
  size_t cap = src_len + 1;
  char *out = malloc(cap ? cap : 1);
  if (!out)
    return false;
  size_t len = 0;
  while (*src) {
    char ch = *src++;
    if (ch == quote) {
      out[len] = '\0';
      *end_out = src;
      *out_text = out;
      return true;
    }
    if (ch == '\\') {
      char esc = *src++;
      if (!esc) {
        free(out);
        return false;
      }
      switch (esc) {
      case 'n':
        ch = '\n';
        break;
      case 'r':
        ch = '\r';
        break;
      case 't':
        ch = '\t';
        break;
      case '0':
        ch = '\0';
        break;
      case '\\':
      case '\'':
      case '"':
        ch = esc;
        break;
      default:
        ch = esc;
        break;
      }
    }
    out[len++] = ch;
  }
  free(out);
  return false;
}

static bool ny_parse_tiny_print_arg(const char *src, const char **end_out,
                                    char **out_text) {
  const char *p = ny_skip_ws(src);
  if (!p || !*p)
    return false;
  if (*p == '"' || *p == '\'')
    return ny_decode_tiny_string_literal(p, end_out, out_text);
  static const char *const words[] = {"true", "false", "nil"};
  for (size_t i = 0; i < sizeof(words) / sizeof(words[0]); ++i) {
    size_t word_len = strlen(words[i]);
    if (strncmp(p, words[i], word_len) == 0 &&
        !(isalnum((unsigned char)p[word_len]) || p[word_len] == '_')) {
      *out_text = ny_strdup(words[i]);
      if (!*out_text)
        return false;
      *end_out = p + word_len;
      return true;
    }
  }
  const char *int_end = NULL;
  char *int_text = NULL;
  if (ny_tiny_parse_i64_constant(p, &int_end, &int_text)) {
    const char *after = ny_skip_ws(int_end);
    if (*after == ')' || *after == ',' || *after == '\0') {
      *end_out = int_end;
      *out_text = int_text;
      return true;
    }
    free(int_text);
  }
  const char *start = p;
  if (*p == '+' || *p == '-')
    p++;
  bool saw_digit = false;
  while (isdigit((unsigned char)*p)) {
    saw_digit = true;
    p++;
  }
  if (*p == '.') {
    p++;
    while (isdigit((unsigned char)*p)) {
      saw_digit = true;
      p++;
    }
  }
  if (!saw_digit)
    return false;
  if (*p == 'e' || *p == 'E') {
    const char *exp = p++;
    if (*p == '+' || *p == '-')
      p++;
    bool exp_digit = false;
    while (isdigit((unsigned char)*p)) {
      exp_digit = true;
      p++;
    }
    if (!exp_digit)
      p = exp;
  }
  *out_text = ny_strndup(start, (size_t)(p - start));
  if (!*out_text)
    return false;
  *end_out = p;
  return true;
}

typedef enum {
  NY_TINY_CMD_NONE = 0,
  NY_TINY_CMD_NOOP,
  NY_TINY_CMD_PRINT,
  NY_TINY_CMD_EPRINT,
} ny_tiny_cmd_kind_t;

typedef struct {
  ny_tiny_cmd_kind_t kind;
  char *text;
} ny_tiny_command_t;

typedef struct {
  char *name;
  int64_t value;
  bool is_mutable;
} ny_tiny_binding_t;

typedef struct {
  ny_tiny_command_t *items;
  size_t len;
  size_t cap;
  ny_tiny_binding_t *bindings;
  size_t binding_len;
  size_t binding_cap;
} ny_tiny_program_t;

static void ny_tiny_command_free(ny_tiny_command_t *cmd) {
  if (!cmd)
    return;
  free(cmd->text);
  cmd->text = NULL;
  cmd->kind = NY_TINY_CMD_NONE;
}

static void ny_tiny_program_free(ny_tiny_program_t *program) {
  if (!program)
    return;
  for (size_t i = 0; i < program->len; ++i)
    ny_tiny_command_free(&program->items[i]);
  for (size_t i = 0; i < program->binding_len; ++i)
    free(program->bindings[i].name);
  free(program->items);
  free(program->bindings);
  *program = (ny_tiny_program_t){0};
}

static bool ny_tiny_text_append(char **text, size_t *len, size_t *cap,
                                const char *part, bool separator) {
  if (!text || !len || !cap || !part)
    return false;
  size_t part_len = strlen(part);
  size_t extra = part_len + (separator ? 1u : 0u);
  if (extra < part_len || *len > SIZE_MAX - extra - 1)
    return false;
  size_t want = *len + extra + 1;
  if (want > *cap) {
    size_t next = *cap ? *cap : 32;
    while (next < want) {
      if (next > SIZE_MAX / 2) {
        next = want;
        break;
      }
      next *= 2;
    }
    char *grown = realloc(*text, next);
    if (!grown)
      return false;
    *text = grown;
    *cap = next;
  }
  if (separator)
    (*text)[(*len)++] = ' ';
  memcpy(*text + *len, part, part_len);
  *len += part_len;
  (*text)[*len] = '\0';
  return true;
}

static bool ny_tiny_binding_lookup(const ny_tiny_program_t *program,
                                   const char *name, size_t name_len,
                                   int64_t *out) {
  if (!program || !name || !out)
    return false;
  for (size_t i = 0; i < program->binding_len; ++i) {
    const char *candidate = program->bindings[i].name;
    if (strlen(candidate) == name_len &&
        memcmp(candidate, name, name_len) == 0) {
      *out = program->bindings[i].value;
      return true;
    }
  }
  return false;
}

static char *ny_tiny_expand_bindings(const ny_tiny_program_t *program,
                                     const char *src) {
  if (!src)
    return NULL;
  char *expanded = NULL;
  size_t len = 0, cap = 0;
  const char *p = src;
  while (*p) {
    if (*p == '"' || *p == '\'') {
      const char *start = p;
      char quote = *p++;
      while (*p) {
        if (*p == '\\' && p[1]) {
          p += 2;
          continue;
        }
        if (*p++ == quote)
          break;
      }
      char *part = ny_strndup(start, (size_t)(p - start));
      bool ok = part && ny_tiny_text_append(&expanded, &len, &cap, part, false);
      free(part);
      if (!ok)
        goto fail;
      continue;
    }
    if (isalpha((unsigned char)*p) || *p == '_') {
      const char *start = p++;
      while (isalnum((unsigned char)*p) || *p == '_')
        ++p;
      int64_t value = 0;
      if (ny_tiny_binding_lookup(program, start, (size_t)(p - start), &value)) {
        char number[32];
        int n = snprintf(number, sizeof(number), "%" PRId64, value);
        if (n < 0 || (size_t)n >= sizeof(number) ||
            !ny_tiny_text_append(&expanded, &len, &cap, number, false))
          goto fail;
      } else {
        char *name = ny_strndup(start, (size_t)(p - start));
        bool ok =
            name && ny_tiny_text_append(&expanded, &len, &cap, name, false);
        free(name);
        if (!ok)
          goto fail;
      }
      continue;
    }
    char one[2] = {*p++, '\0'};
    if (!ny_tiny_text_append(&expanded, &len, &cap, one, false))
      goto fail;
  }
  if (!expanded)
    return ny_strdup("");
  return expanded;

fail:
  free(expanded);
  return NULL;
}

static bool ny_tiny_program_add_binding(ny_tiny_program_t *program,
                                        const char *name, size_t name_len,
                                        int64_t value, bool is_mutable) {
  int64_t ignored = 0;
  if (!program || !name || name_len == 0 ||
      ny_tiny_binding_lookup(program, name, name_len, &ignored))
    return false;
  if (program->binding_len == program->binding_cap) {
    size_t cap = program->binding_cap ? program->binding_cap * 2 : 4;
    if (cap < program->binding_cap ||
        cap > SIZE_MAX / sizeof(*program->bindings))
      return false;
    ny_tiny_binding_t *bindings =
        realloc(program->bindings, cap * sizeof(*program->bindings));
    if (!bindings)
      return false;
    program->bindings = bindings;
    program->binding_cap = cap;
  }
  char *copy = ny_strndup(name, name_len);
  if (!copy)
    return false;
  program->bindings[program->binding_len++] = (ny_tiny_binding_t){
      .name = copy, .value = value, .is_mutable = is_mutable};
  return true;
}

static bool ny_tiny_program_set_binding(ny_tiny_program_t *program,
                                        const char *name, size_t name_len,
                                        int64_t value) {
  if (!program || !name)
    return false;
  for (size_t i = 0; i < program->binding_len; ++i) {
    ny_tiny_binding_t *binding = &program->bindings[i];
    if (strlen(binding->name) == name_len &&
        memcmp(binding->name, name, name_len) == 0) {
      if (!binding->is_mutable)
        return false;
      binding->value = value;
      return true;
    }
  }
  return false;
}

static bool ny_tiny_is_binding_statement(const char *src) {
  const char *p = ny_skip_ws(src);
  bool is_def =
      strncmp(p, "def", 3) == 0 && !isalnum((unsigned char)p[3]) && p[3] != '_';
  bool is_mut =
      strncmp(p, "mut", 3) == 0 && !isalnum((unsigned char)p[3]) && p[3] != '_';
  return is_def || is_mut;
}

static bool ny_tiny_parse_binding_statement(ny_tiny_program_t *program,
                                            const char *src) {
  const char *p = ny_skip_ws(src);
  bool is_mut =
      strncmp(p, "mut", 3) == 0 && !isalnum((unsigned char)p[3]) && p[3] != '_';
  bool is_def =
      strncmp(p, "def", 3) == 0 && !isalnum((unsigned char)p[3]) && p[3] != '_';
  if (!is_mut && !is_def)
    return false;
  p = ny_skip_ws(p + 3);
  if (strncmp(p, "int", 3) == 0 &&
      !(isalnum((unsigned char)p[3]) || p[3] == '_'))
    p = ny_skip_ws(p + 3);
  if (!(isalpha((unsigned char)*p) || *p == '_'))
    return false;
  const char *name = p++;
  while (isalnum((unsigned char)*p) || *p == '_')
    ++p;
  size_t name_len = (size_t)(p - name);
  p = ny_skip_ws(p);
  if (*p++ != '=')
    return false;
  p = ny_skip_ws(p);
  char *expanded = ny_tiny_expand_bindings(program, p);
  if (!expanded)
    return false;
  const char *end = expanded;
  char *value_text = NULL;
  bool ok = ny_tiny_parse_i64_constant(expanded, &end, &value_text) &&
            *ny_skip_ws(end) == '\0';
  if (ok) {
    int64_t value = (int64_t)strtoll(value_text, NULL, 10);
    ok = ny_tiny_program_add_binding(program, name, name_len, value, is_mut);
  }
  free(value_text);
  free(expanded);
  return ok;
}

static bool ny_tiny_parse_assignment_statement(ny_tiny_program_t *program,
                                               const char *src,
                                               bool *recognized) {
  if (recognized)
    *recognized = false;
  const char *p = ny_skip_ws(src);
  if (!(isalpha((unsigned char)*p) || *p == '_'))
    return false;
  const char *name = p++;
  while (isalnum((unsigned char)*p) || *p == '_')
    ++p;
  size_t name_len = (size_t)(p - name);
  p = ny_skip_ws(p);
  char op = '\0';
  if (*p == '=' && p[1] != '=') {
    op = '=';
    ++p;
  } else if ((*p == '+' || *p == '-' || *p == '*' || *p == '/' || *p == '%') &&
             p[1] == '=') {
    op = *p;
    p += 2;
  } else {
    return false;
  }
  if (recognized)
    *recognized = true;
  int64_t lhs = 0;
  if (!ny_tiny_binding_lookup(program, name, name_len, &lhs))
    return false;
  p = ny_skip_ws(p);
  char *expanded = ny_tiny_expand_bindings(program, p);
  if (!expanded)
    return false;
  const char *end = expanded;
  char *rhs_text = NULL;
  bool ok = ny_tiny_parse_i64_constant(expanded, &end, &rhs_text) &&
            *ny_skip_ws(end) == '\0';
  if (ok) {
    int64_t rhs = (int64_t)strtoll(rhs_text, NULL, 10);
    int64_t result = rhs;
    switch (op) {
    case '+':
      ok = ny_tiny_i64_add(lhs, rhs, &result);
      break;
    case '-':
      ok = ny_tiny_i64_sub(lhs, rhs, &result);
      break;
    case '*':
      ok = ny_tiny_i64_mul(lhs, rhs, &result);
      break;
    case '/':
    case '%':
      if (rhs == 0 || (lhs == INT64_MIN && rhs == -1))
        ok = false;
      else
        result = op == '/' ? lhs / rhs : lhs % rhs;
      break;
    default:
      break;
    }
    if (ok)
      ok = ny_tiny_program_set_binding(program, name, name_len, result);
  }
  free(rhs_text);
  free(expanded);
  return ok;
}

static bool ny_parse_tiny_command_string(const char *src,
                                         ny_tiny_command_t *out) {
  if (!src || !out)
    return false;
  memset(out, 0, sizeof(*out));
  const char *p = ny_skip_ws(src);
  ny_tiny_cmd_kind_t kind = NY_TINY_CMD_NOOP;
  bool wrapped_call = false;
  if (strncmp(p, "print", 5) == 0 && !isalnum((unsigned char)p[5]) &&
      p[5] != '_') {
    p += 5;
    kind = NY_TINY_CMD_PRINT;
    wrapped_call = true;
  } else if (strncmp(p, "eprint", 6) == 0 && !isalnum((unsigned char)p[6]) &&
             p[6] != '_') {
    p += 6;
    kind = NY_TINY_CMD_EPRINT;
    wrapped_call = true;
  }
  if (wrapped_call) {
    p = ny_skip_ws(p);
    if (*p != '(')
      return false;
    p++;
  }

  char *text = NULL;
  if (wrapped_call) {
    size_t text_len = 0, text_cap = 0;
    bool first = true;
    for (;;) {
      char *arg = NULL;
      if (!ny_parse_tiny_print_arg(p, &p, &arg) ||
          !ny_tiny_text_append(&text, &text_len, &text_cap, arg, !first)) {
        free(arg);
        free(text);
        return false;
      }
      free(arg);
      first = false;
      p = ny_skip_ws(p);
      if (*p == ',') {
        p = ny_skip_ws(p + 1);
        continue;
      }
      if (*p != ')') {
        free(text);
        return false;
      }
      p++;
      break;
    }
  } else if (!ny_parse_tiny_print_arg(p, &p, &text)) {
    return false;
  }
  p = ny_skip_ws(p);
  if (*p == ';') {
    p++;
    p = ny_skip_ws(p);
  }
  if (*p != '\0') {
    free(text);
    return false;
  }
  out->kind = kind;
  out->text = text;
  return true;
}

static bool ny_tiny_program_push(ny_tiny_program_t *program,
                                 ny_tiny_command_t *cmd) {
  if (program->len == program->cap) {
    size_t cap = program->cap ? program->cap * 2 : 4;
    if (cap < program->cap || cap > SIZE_MAX / sizeof(*program->items))
      return false;
    ny_tiny_command_t *items =
        realloc(program->items, cap * sizeof(*program->items));
    if (!items)
      return false;
    program->items = items;
    program->cap = cap;
  }
  program->items[program->len++] = *cmd;
  *cmd = (ny_tiny_command_t){0};
  return true;
}

static bool ny_tiny_program_add_statement(ny_tiny_program_t *program,
                                          const char *src, size_t len) {
  char *statement = ny_strndup(src, len);
  if (!statement)
    return false;
  if (!*ny_skip_ws(statement)) {
    free(statement);
    return true;
  }
  const char *trimmed = ny_skip_ws(statement);
  if (ny_tiny_is_binding_statement(trimmed)) {
    bool added = ny_tiny_parse_binding_statement(program, trimmed);
    free(statement);
    return added;
  }
  bool assignment_recognized = false;
  bool assigned = ny_tiny_parse_assignment_statement(program, trimmed,
                                                     &assignment_recognized);
  if (assignment_recognized) {
    free(statement);
    return assigned;
  }
  char *expanded = ny_tiny_expand_bindings(program, statement);
  if (!expanded) {
    free(statement);
    return false;
  }
  ny_tiny_command_t cmd = {0};
  bool parsed = ny_parse_tiny_command_string(expanded, &cmd);
  free(expanded);
  free(statement);
  if (!parsed || !ny_tiny_program_push(program, &cmd)) {
    ny_tiny_command_free(&cmd);
    return false;
  }
  return true;
}

/*
 * Bare AOT accepts a deliberately small source subset: constant integer
 * bindings/assignments and independent output statements. Split at top-level
 * newlines; a semicolon begins a line comment, matching the language lexer.
 * String contents and print(...) parentheses stay intact.
 */
static bool __attribute__((unused))
ny_parse_tiny_program(const char *src, ny_tiny_program_t *out) {
  if (!src || !out)
    return false;
  *out = (ny_tiny_program_t){0};
  const char *start = src;
  char quote = '\0';
  bool escaped = false;
  unsigned depth = 0;
  for (const char *p = src;; ++p) {
    char ch = *p;
    if (quote) {
      if (escaped) {
        escaped = false;
      } else if (ch == '\\') {
        escaped = true;
      } else if (ch == quote || ch == '\0') {
        quote = '\0';
      }
    } else if (ch == '"' || ch == '\'') {
      quote = ch;
    } else if (ch == '(') {
      if (depth == UINT_MAX)
        goto fail;
      ++depth;
    } else if (ch == ')' && depth > 0) {
      --depth;
    }
    if (ch == ';' && !quote && depth == 0) {
      if (!ny_tiny_program_add_statement(out, start, (size_t)(p - start)))
        goto fail;
      const char *line_end = strchr(p, '\n');
      if (!line_end)
        break;
      start = line_end + 1;
      p = line_end;
      continue;
    }
    bool boundary = ch == '\0' || (ch == '\n' && !quote && depth == 0);
    if (!boundary)
      continue;
    if (!ny_tiny_program_add_statement(out, start, (size_t)(p - start)))
      goto fail;
    if (!ch)
      break;
    start = p + 1;
  }
  return true;
fail:
  ny_tiny_program_free(out);
  return false;
}

static char *ny_c_escape_string_literal(const char *src) {
  if (!src)
    src = "";
  size_t src_len = strlen(src);
  if (src_len > SIZE_MAX / 4)
    return NULL;
  size_t cap = src_len * 4 + 1;
  char *out = malloc(cap ? cap : 1);
  if (!out)
    return NULL;
  size_t len = 0;
  for (const unsigned char *p = (const unsigned char *)src; *p; ++p) {
    unsigned char ch = *p;
    switch (ch) {
    case '\\':
      out[len++] = '\\';
      out[len++] = '\\';
      break;
    case '"':
      out[len++] = '\\';
      out[len++] = '"';
      break;
    case '\n':
      out[len++] = '\\';
      out[len++] = 'n';
      break;
    case '\r':
      out[len++] = '\\';
      out[len++] = 'r';
      break;
    case '\t':
      out[len++] = '\\';
      out[len++] = 't';
      break;
    default:
      if (ch < 32 || ch >= 127) {
        snprintf(out + len, cap - len, "\\%03o", ch);
        len += 4;
      } else {
        out[len++] = (char)ch;
      }
      break;
    }
  }
  out[len] = '\0';
  return out;
}

static bool ny_output_path_is_object(const char *path) {
  if (!path)
    return false;
  size_t len = strlen(path);
  return (len > 2 && strcmp(path + len - 2, ".o") == 0) ||
         (len > 4 && strcmp(path + len - 4, ".obj") == 0);
}

#if defined(__linux__)
static void ny_emit_u32le(unsigned char *dst, uint32_t v) {
  dst[0] = (unsigned char)(v & 0xffu);
  dst[1] = (unsigned char)((v >> 8) & 0xffu);
  dst[2] = (unsigned char)((v >> 16) & 0xffu);
  dst[3] = (unsigned char)((v >> 24) & 0xffu);
}

static bool ny_write_linux_x64_tiny_exe(const ny_options *opt,
                                        const ny_tiny_command_t *cmd) {
  if (!opt || !opt->output_file || !cmd)
    return false;
  bool do_write =
      (cmd->kind == NY_TINY_CMD_PRINT || cmd->kind == NY_TINY_CMD_EPRINT);
  const char *text = cmd->text ? cmd->text : "";
  size_t text_len = do_write ? strlen(text) : 0;
  if (text_len > UINT32_MAX - 1u)
    return false;
  size_t msg_len = do_write ? text_len + 1u : 0u;

  unsigned char code[64];
  size_t c = 0;
  size_t disp_pos = 0;
  if (do_write) {
    code[c++] = 0x31;
    code[c++] = 0xc0;
    code[c++] = 0xb0;
    code[c++] = 1;
    code[c++] = 0xbf;
    ny_emit_u32le(code + c, cmd->kind == NY_TINY_CMD_EPRINT ? 2u : 1u);
    c += 4;
    code[c++] = 0x48;
    code[c++] = 0x8d;
    code[c++] = 0x35;
    disp_pos = c;
    c += 4;
    code[c++] = 0xba;
    ny_emit_u32le(code + c, (uint32_t)msg_len);
    c += 4;
    code[c++] = 0x0f;
    code[c++] = 0x05;
  }
  code[c++] = 0x31;
  code[c++] = 0xff;
  code[c++] = 0x31;
  code[c++] = 0xc0;
  code[c++] = 0xb0;
  code[c++] = 60;
  code[c++] = 0x0f;
  code[c++] = 0x05;

  const uint64_t base = UINT64_C(0x400000);
  const size_t code_off = sizeof(Elf64_Ehdr) + sizeof(Elf64_Phdr);
  if (do_write) {
    size_t msg_off = code_off + c;
    size_t rip_after_lea = code_off + disp_pos + 4u;
    int64_t disp = (int64_t)msg_off - (int64_t)rip_after_lea;
    if (disp < INT32_MIN || disp > INT32_MAX)
      return false;
    ny_emit_u32le(code + disp_pos, (uint32_t)(int32_t)disp);
  }

  Elf64_Ehdr eh;
  memset(&eh, 0, sizeof(eh));
  eh.e_ident[EI_MAG0] = ELFMAG0;
  eh.e_ident[EI_MAG1] = ELFMAG1;
  eh.e_ident[EI_MAG2] = ELFMAG2;
  eh.e_ident[EI_MAG3] = ELFMAG3;
  eh.e_ident[EI_CLASS] = ELFCLASS64;
  eh.e_ident[EI_DATA] = ELFDATA2LSB;
  eh.e_ident[EI_VERSION] = EV_CURRENT;
  eh.e_ident[EI_OSABI] = ELFOSABI_SYSV;
  eh.e_type = ET_EXEC;
  eh.e_machine = EM_X86_64;
  eh.e_version = EV_CURRENT;
  eh.e_entry = base + code_off;
  eh.e_phoff = sizeof(Elf64_Ehdr);
  eh.e_ehsize = sizeof(Elf64_Ehdr);
  eh.e_phentsize = sizeof(Elf64_Phdr);
  eh.e_phnum = 1;

  Elf64_Phdr ph;
  memset(&ph, 0, sizeof(ph));
  ph.p_type = PT_LOAD;
  ph.p_flags = PF_R | PF_X;
  ph.p_offset = 0;
  ph.p_vaddr = base;
  ph.p_paddr = base;
  ph.p_filesz = code_off + c + msg_len;
  ph.p_memsz = ph.p_filesz;
  ph.p_align = 0x1000;

  ny_ensure_parent_dir_for_path(opt->output_file);
  FILE *f = fopen(opt->output_file, "wb");
  if (!f)
    return false;
  bool ok = fwrite(&eh, 1, sizeof(eh), f) == sizeof(eh) &&
            fwrite(&ph, 1, sizeof(ph), f) == sizeof(ph) &&
            fwrite(code, 1, c, f) == c;
  if (ok && do_write) {
    ok = fwrite(text, 1, text_len, f) == text_len && fputc('\n', f) != EOF;
  }
  if (fclose(f) != 0)
    ok = false;
  if (!ok) {
    unlink(opt->output_file);
    return false;
  }
  chmod(opt->output_file, 0755);
  return true;
}

static bool
ny_write_linux_x64_tiny_program_exe(const ny_options *opt,
                                    const ny_tiny_program_t *program) {
  if (!opt || !opt->output_file || !program)
    return false;
  size_t writes = 0, data_len = 0;
  for (size_t i = 0; i < program->len; ++i) {
    const ny_tiny_command_t *cmd = &program->items[i];
    if (cmd->kind != NY_TINY_CMD_PRINT && cmd->kind != NY_TINY_CMD_EPRINT)
      continue;
    size_t len = strlen(cmd->text ? cmd->text : "");
    if (len >= UINT32_MAX || data_len > SIZE_MAX - len - 1)
      return false;
    ++writes;
    data_len += len + 1;
  }
  if (writes > (SIZE_MAX - 8) / 23)
    return false;
  size_t code_len = writes * 23 + 8;
  size_t code_off = sizeof(Elf64_Ehdr) + sizeof(Elf64_Phdr);
  if (code_len > SIZE_MAX - code_off ||
      data_len > SIZE_MAX - code_off - code_len)
    return false;
  unsigned char *code = calloc(code_len, 1);
  unsigned char *data = data_len ? malloc(data_len) : NULL;
  if (!code || (data_len && !data)) {
    free(code);
    free(data);
    return false;
  }

  size_t c = 0, d = 0;
  for (size_t i = 0; i < program->len; ++i) {
    const ny_tiny_command_t *cmd = &program->items[i];
    if (cmd->kind != NY_TINY_CMD_PRINT && cmd->kind != NY_TINY_CMD_EPRINT)
      continue;
    const char *text = cmd->text ? cmd->text : "";
    size_t text_len = strlen(text);
    code[c++] = 0x31;
    code[c++] = 0xc0;
    code[c++] = 0xb0;
    code[c++] = 1;
    code[c++] = 0xbf;
    ny_emit_u32le(code + c, cmd->kind == NY_TINY_CMD_EPRINT ? 2 : 1);
    c += 4;
    code[c++] = 0x48;
    code[c++] = 0x8d;
    code[c++] = 0x35;
    size_t disp_pos = c;
    c += 4;
    code[c++] = 0xba;
    uint32_t msg_len = (uint32_t)(text_len + 1);
    ny_emit_u32le(code + c, msg_len);
    c += 4;
    code[c++] = 0x0f;
    code[c++] = 0x05;
    size_t rip_after_lea = code_off + disp_pos + 4;
    size_t msg_off = code_off + code_len + d;
    int64_t disp = (int64_t)msg_off - (int64_t)rip_after_lea;
    if (disp < INT32_MIN || disp > INT32_MAX) {
      free(code);
      free(data);
      return false;
    }
    ny_emit_u32le(code + disp_pos, (uint32_t)(int32_t)disp);
    memcpy(data + d, text, text_len);
    data[d + text_len] = '\n';
    d += text_len + 1;
  }
  code[c++] = 0x31;
  code[c++] = 0xff;
  code[c++] = 0x31;
  code[c++] = 0xc0;
  code[c++] = 0xb0;
  code[c++] = 60;
  code[c++] = 0x0f;
  code[c++] = 0x05;
  if (c != code_len || d != data_len) {
    free(code);
    free(data);
    return false;
  }

  const uint64_t base = UINT64_C(0x400000);
  Elf64_Ehdr eh = {0};
  eh.e_ident[EI_MAG0] = ELFMAG0;
  eh.e_ident[EI_MAG1] = ELFMAG1;
  eh.e_ident[EI_MAG2] = ELFMAG2;
  eh.e_ident[EI_MAG3] = ELFMAG3;
  eh.e_ident[EI_CLASS] = ELFCLASS64;
  eh.e_ident[EI_DATA] = ELFDATA2LSB;
  eh.e_ident[EI_VERSION] = EV_CURRENT;
  eh.e_ident[EI_OSABI] = ELFOSABI_SYSV;
  eh.e_type = ET_EXEC;
  eh.e_machine = EM_X86_64;
  eh.e_version = EV_CURRENT;
  eh.e_entry = base + code_off;
  eh.e_phoff = sizeof(Elf64_Ehdr);
  eh.e_ehsize = sizeof(Elf64_Ehdr);
  eh.e_phentsize = sizeof(Elf64_Phdr);
  eh.e_phnum = 1;
  Elf64_Phdr ph = {0};
  ph.p_type = PT_LOAD;
  ph.p_flags = PF_R | PF_X;
  ph.p_vaddr = ph.p_paddr = base;
  ph.p_filesz = ph.p_memsz = code_off + code_len + data_len;
  ph.p_align = 0x1000;

  ny_ensure_parent_dir_for_path(opt->output_file);
  FILE *f = fopen(opt->output_file, "wb");
  if (!f) {
    free(code);
    free(data);
    return false;
  }
  bool ok = fwrite(&eh, 1, sizeof(eh), f) == sizeof(eh) &&
            fwrite(&ph, 1, sizeof(ph), f) == sizeof(ph) &&
            fwrite(code, 1, code_len, f) == code_len &&
            (!data_len || fwrite(data, 1, data_len, f) == data_len);
  if (fclose(f) != 0)
    ok = false;
  free(code);
  free(data);
  if (!ok) {
    unlink(opt->output_file);
    return false;
  }
  chmod(opt->output_file, 0755);
  return true;
}

static bool
ny_write_linux_aarch64_tiny_program_exe(const ny_options *opt,
                                        const ny_tiny_program_t *program) {
  if (!opt || !opt->output_file || !program)
    return false;
  size_t writes = 0, data_len = 0;
  for (size_t i = 0; i < program->len; ++i) {
    const ny_tiny_command_t *cmd = &program->items[i];
    if (cmd->kind != NY_TINY_CMD_PRINT && cmd->kind != NY_TINY_CMD_EPRINT)
      continue;
    size_t len = strlen(cmd->text ? cmd->text : "");
    if (len >= UINT32_MAX || data_len > SIZE_MAX - len - 1 ||
        writes == (SIZE_MAX - 12) / 24)
      return false;
    ++writes;
    data_len += len + 1;
  }
  if (writes > (SIZE_MAX - 12) / 24)
    return false;
  size_t code_len = writes * 24 + 12;
  size_t code_off = sizeof(Elf64_Ehdr) + sizeof(Elf64_Phdr);
  if (code_len > SIZE_MAX - code_off ||
      data_len > SIZE_MAX - code_off - code_len)
    return false;
  unsigned char *code = calloc(code_len, 1);
  unsigned char *data = data_len ? malloc(data_len) : NULL;
  if (!code || (data_len && !data)) {
    free(code);
    free(data);
    return false;
  }

  size_t c = 0, d = 0;
  for (size_t i = 0; i < program->len; ++i) {
    const ny_tiny_command_t *cmd = &program->items[i];
    if (cmd->kind != NY_TINY_CMD_PRINT && cmd->kind != NY_TINY_CMD_EPRINT)
      continue;
    const char *text = cmd->text ? cmd->text : "";
    size_t text_len = strlen(text);
    ny_emit_u32le(code + c,
                  0xd2800000u |
                      ((cmd->kind == NY_TINY_CMD_EPRINT ? 2u : 1u) << 5));
    c += 4;
    size_t adr_pos = c;
    int64_t adr_delta = (int64_t)(code_len + d) - (int64_t)adr_pos;
    if (adr_delta < -(INT64_C(1) << 20) || adr_delta >= (INT64_C(1) << 20)) {
      free(code);
      free(data);
      return false;
    }
    uint64_t adr_imm = (uint64_t)adr_delta & ((UINT64_C(1) << 21) - 1);
    ny_emit_u32le(code + c, 0x10000001u | (uint32_t)((adr_imm & 3u) << 29) |
                                (uint32_t)(((adr_imm >> 2) & 0x7ffffu) << 5));
    c += 4;
    uint32_t msg_len = (uint32_t)(text_len + 1);
    ny_emit_u32le(code + c, 0xd2800002u | ((msg_len & 0xffffu) << 5));
    c += 4;
    ny_emit_u32le(code + c, 0xf2a00002u | (((msg_len >> 16) & 0xffffu) << 5));
    c += 4;
    ny_emit_u32le(code + c, 0xd2800808u);
    c += 4;
    ny_emit_u32le(code + c, 0xd4000001u);
    c += 4;
    memcpy(data + d, text, text_len);
    data[d + text_len] = '\n';
    d += text_len + 1;
  }
  ny_emit_u32le(code + c, 0xaa1f03e0u);
  c += 4;
  ny_emit_u32le(code + c, 0xd2800ba8u);
  c += 4;
  ny_emit_u32le(code + c, 0xd4000001u);
  c += 4;
  if (c != code_len || d != data_len) {
    free(code);
    free(data);
    return false;
  }

  const uint64_t base = UINT64_C(0x400000);
  Elf64_Ehdr eh = {0};
  eh.e_ident[EI_MAG0] = ELFMAG0;
  eh.e_ident[EI_MAG1] = ELFMAG1;
  eh.e_ident[EI_MAG2] = ELFMAG2;
  eh.e_ident[EI_MAG3] = ELFMAG3;
  eh.e_ident[EI_CLASS] = ELFCLASS64;
  eh.e_ident[EI_DATA] = ELFDATA2LSB;
  eh.e_ident[EI_VERSION] = EV_CURRENT;
  eh.e_ident[EI_OSABI] = ELFOSABI_SYSV;
  eh.e_type = ET_EXEC;
  eh.e_machine = EM_AARCH64;
  eh.e_version = EV_CURRENT;
  eh.e_entry = base + code_off;
  eh.e_phoff = sizeof(Elf64_Ehdr);
  eh.e_ehsize = sizeof(Elf64_Ehdr);
  eh.e_phentsize = sizeof(Elf64_Phdr);
  eh.e_phnum = 1;
  Elf64_Phdr ph = {0};
  ph.p_type = PT_LOAD;
  ph.p_flags = PF_R | PF_X;
  ph.p_vaddr = ph.p_paddr = base;
  ph.p_filesz = ph.p_memsz = code_off + code_len + data_len;
  ph.p_align = 0x1000;

  ny_ensure_parent_dir_for_path(opt->output_file);
  FILE *f = fopen(opt->output_file, "wb");
  if (!f) {
    free(code);
    free(data);
    return false;
  }
  bool ok = fwrite(&eh, 1, sizeof(eh), f) == sizeof(eh) &&
            fwrite(&ph, 1, sizeof(ph), f) == sizeof(ph) &&
            fwrite(code, 1, code_len, f) == code_len &&
            (!data_len || fwrite(data, 1, data_len, f) == data_len);
  if (fclose(f) != 0)
    ok = false;
  free(code);
  free(data);
  if (!ok) {
    unlink(opt->output_file);
    return false;
  }
  chmod(opt->output_file, 0755);
  return true;
}
#endif

static bool ny_tiny_target_is_aarch64(const ny_options *opt) {
  if (!opt)
    return false;
  if (opt->native_backend == NY_NATIVE_BACKEND_AARCH64)
    return true;
  if (opt->native_backend == NY_NATIVE_BACKEND_X86_64)
    return false;
  if (opt->host_triple && (strstr(opt->host_triple, "aarch64") ||
                           strstr(opt->host_triple, "arm64")))
    return true;
#if defined(__aarch64__)
  return true;
#else
  return false;
#endif
}

static bool ny_tiny_aot_cache_path(const ny_options *opt, bool object_only,
                                   char *out, size_t out_len) {
  if (!opt || !out || out_len == 0)
    return false;
  const char *root = ny_cache_root_dir();
  if (!root || !*root)
    return false;
  uint64_t h = NY_FNV1A64_OFFSET_BASIS;
  h = ny_fnv1a64_cstr("tiny-aot-v6", h);
  h = ny_fnv1a64_cstr(opt->command_string ? opt->command_string : "", h);
  h = ny_fnv1a64_cstr(ny_builder_choose_cc(), h);
  h = ny_fnv1a64(&opt->native_backend, sizeof(opt->native_backend), h);
  h = ny_fnv1a64_cstr(opt->host_triple ? opt->host_triple : "", h);
  bool aarch64 = ny_tiny_target_is_aarch64(opt);
  h = ny_fnv1a64(&aarch64, sizeof(aarch64), h);
  h = ny_fnv1a64(&object_only, sizeof(object_only), h);
  h = ny_fnv1a64(&opt->strip_override, sizeof(opt->strip_override), h);
  char dir[4096];
  snprintf(dir, sizeof(dir), "%s/tiny-aot", root);
  ny_ensure_dir_recursive(dir);
  snprintf(out, out_len, "%s/tiny_%016" PRIx64 "%s", dir, h,
           object_only ? ".o" : ".bin");
  return true;
}

static bool ny_write_tiny_c_source(const char *src_path,
                                   const ny_tiny_command_t *cmd) {
  if (!src_path || !cmd)
    return false;
  FILE *f = fopen(src_path, "wb");
  if (!f)
    return false;

  char *escaped = NULL;
  if (cmd->kind == NY_TINY_CMD_PRINT || cmd->kind == NY_TINY_CMD_EPRINT) {
    escaped = ny_c_escape_string_literal(cmd->text);
    if (!escaped) {
      fclose(f);
      unlink(src_path);
      return false;
    }
  }

  fputs("#include <stdio.h>\nint main(void) {\n", f);
  if (cmd->kind == NY_TINY_CMD_PRINT || cmd->kind == NY_TINY_CMD_EPRINT) {
    const char *stream = cmd->kind == NY_TINY_CMD_EPRINT ? "stderr" : "stdout";
    fprintf(f, "  fputs(\"%s\", %s);\n", escaped, stream);
    fprintf(f, "  fputc('\\n', %s);\n", stream);
  }
  fputs("  return 0;\n}\n", f);
  free(escaped);
  if (fclose(f) != 0) {
    unlink(src_path);
    return false;
  }
  return true;
}

static bool ny_compile_tiny_c_fallback(const ny_options *opt, bool object_only,
                                       const char *src_path) {
  if (!opt || !opt->output_file || !src_path)
    return false;
  ny_ensure_parent_dir_for_path(opt->output_file);
  const char *cc = ny_builder_choose_cc();
  const char *argv_exe[] = {cc,   "-std=c11",       "-O0", "-x", "c", src_path,
                            "-o", opt->output_file, NULL};
  const char *argv_obj[] = {
      cc,   "-std=c11",       "-O0", "-x", "c", "-c", src_path,
      "-o", opt->output_file, NULL};
  int rc = ny_exec_spawn(object_only ? argv_obj : argv_exe);
  if (rc != 0)
    return false;
  if (!object_only && opt->strip_override == 1)
    (void)ny_builder_strip(opt->output_file);
  return true;
}

static bool ny_compile_tiny_command(const ny_options *opt,
                                    const ny_tiny_command_t *cmd) {
  if (!opt || !opt->output_file || !cmd)
    return false;
  bool object_only = ny_output_path_is_object(opt->output_file);
  char cache_path[4096] = {0};
  bool have_cache_path =
      !opt->input_file &&
      ny_tiny_aot_cache_path(opt, object_only, cache_path, sizeof(cache_path));
  if (have_cache_path && ny_access(cache_path, R_OK) == 0) {
    ny_ensure_parent_dir_for_path(opt->output_file);
    if (ny_copy_file(cache_path, opt->output_file) == 0)
      return true;
  }

#if defined(__linux__)
  if (!object_only &&
      (ny_tiny_target_is_aarch64(opt)
           ? ny_write_linux_aarch64_tiny_program_exe(
                 opt, &(ny_tiny_program_t){.items = (ny_tiny_command_t *)cmd,
                                           .len = 1})
           : ny_write_linux_x64_tiny_exe(opt, cmd))) {
    if (have_cache_path)
      (void)ny_copy_file(opt->output_file, cache_path);
    return true;
  }
#endif

  char src_path[4096];
  snprintf(src_path, sizeof(src_path), "%s/ny_tiny_%ld.c", ny_get_temp_dir(),
           (long)getpid());
  if (!ny_write_tiny_c_source(src_path, cmd))
    return false;
  bool ok = ny_compile_tiny_c_fallback(opt, object_only, src_path);
  unlink(src_path);
  if (!ok)
    return false;
  if (have_cache_path)
    (void)ny_copy_file(opt->output_file, cache_path);
  return true;
}

static bool __attribute__((unused))
ny_compile_tiny_program(const ny_options *opt,
                        const ny_tiny_program_t *program) {
#if defined(__linux__)
  if (!opt || !opt->output_file || !program ||
      ny_output_path_is_object(opt->output_file))
    return false;
  char cache_path[4096] = {0};
  bool have_cache_path =
      !opt->input_file &&
      ny_tiny_aot_cache_path(opt, false, cache_path, sizeof(cache_path));
  if (have_cache_path && ny_access(cache_path, R_OK) == 0) {
    ny_ensure_parent_dir_for_path(opt->output_file);
    if (ny_copy_file(cache_path, opt->output_file) == 0)
      return true;
  }
  bool aarch64 = ny_tiny_target_is_aarch64(opt);
  bool emitted = aarch64 ? ny_write_linux_aarch64_tiny_program_exe(opt, program)
                         : ny_write_linux_x64_tiny_program_exe(opt, program);
  if (!emitted)
    return false;
  if (have_cache_path)
    (void)ny_copy_file(opt->output_file, cache_path);
  return true;
#else
  (void)opt;
  (void)program;
  return false;
#endif
}

static void ny_tiny_timing_report(const ny_options *opt, ny_tick_t t_start,
                                  bool aot) {
  if (!opt || !opt->do_timing)
    return;
  if (ny_dbg_json_enabled()) {
    ny_dbg_heuristic_event("PHASE_TIME", "Read file 0.0000s");
    ny_dbg_heuristic_event("PHASE_TIME", "Scan imports 0.0000s");
    ny_dbg_heuristic_event("PHASE_TIME", "Core load 0.0000s");
    ny_dbg_heuristic_event("PHASE_TIME", "Parsing 0.0000s");
    ny_dbg_heuristic_event("PHASE_TIME", "Codegen 0.0000s");
    ny_dbg_heuristic_event(aot ? "PHASE_TIME" : "PHASE_TIME", "%s %.4fs",
                           aot ? "Tiny AOT" : "JIT Compile",
                           aot ? ny_ticks_elapsed_sec(t_start) : 0.0);
    if (!aot)
      ny_dbg_heuristic_event("PHASE_TIME", "JIT Run 0.0000s");
    ny_dbg_heuristic_event("PHASE_TIME", "Total time %.4fs",
                           ny_ticks_elapsed_sec(t_start));
    return;
  }
  fprintf(stderr, "Read file:   0.0000s\n");
  fprintf(stderr, "Scan imports: 0.0000s\n");
  fprintf(stderr, "Core load: 0.0000s\n");
  fprintf(stderr, "Parsing:     0.0000s\n");
  fprintf(stderr, "Codegen:     0.0000s\n");
  if (aot) {
    fprintf(stderr, "Tiny AOT:    %.4fs\n", ny_ticks_elapsed_sec(t_start));
  } else {
    fprintf(stderr, "JIT Init:    0.0000s\n");
    fprintf(stderr, "JIT Compile: 0.0000s\n");
    fprintf(stderr, "JIT Run:     0.0000s\n");
  }
  fprintf(stderr, "Total time:  %.4fs\n", ny_ticks_elapsed_sec(t_start));
}

static bool __attribute__((unused))
ny_bare_source_needs_native(const char *source) {
  for (const char *p = source; p && *p; ++p) {
    bool function =
        strncmp(p, "fn", 2) == 0 &&
        (p == source || (!isalnum((unsigned char)p[-1]) && p[-1] != '_')) &&
        (!isalnum((unsigned char)p[2]) && p[2] != '_');
    bool use_import =
        strncmp(p, "use", 3) == 0 &&
        (p == source || (!isalnum((unsigned char)p[-1]) && p[-1] != '_')) &&
        (!isalnum((unsigned char)p[3]) && p[3] != '_');
    bool print_call =
        strncmp(p, "print", 5) == 0 &&
        (p == source || (!isalnum((unsigned char)p[-1]) && p[-1] != '_')) &&
        p[5] == '(';
    bool eprint_call =
        strncmp(p, "eprint", 6) == 0 &&
        (p == source || (!isalnum((unsigned char)p[-1]) && p[-1] != '_')) &&
        p[6] == '(';
    if (function || use_import || print_call || eprint_call)
      return true;
  }
  return false;
}

static const char *ny_bare_aot_unsupported_inspection(const ny_options *opt) {
  if (opt->emit_asm_path)
    return "--emit-asm";
  if (opt->emit_ir_path)
    return "--emit-ir";
  if (opt->emit_bc_path)
    return "--emit-bc";
  if (opt->emit_report_path)
    return "--emit-report";
  if (opt->emit_artifact_path)
    return "--emit-artifact";
  if (opt->emit_shapes)
    return "--emit-shapes";
  if (opt->emit_refinement_interface_path)
    return "--emit-refinement-interface";
  if (opt->emit_effect_interface_path)
    return "--emit-effect-interface";
  if (opt->native_profile_use_path)
    return "--native-profile-use";
  if (opt->dump_llvm)
    return "--dump-llvm";
  if (opt->dump_funcs)
    return "--dump-funcs";
  if (opt->nyr_dump_text || opt->nyr_dump_raw || opt->nyr_dump_cfg ||
      opt->nyr_dump_stats || opt->nyr_dump_bin)
    return "--nyr-dump";
  if (opt->vm_disasm)
    return "--vm-disasm";
  if (opt->verbosity > 0)
    return "-v";
  if (opt->dbg_stack_layout)
    return "--stack-frame";
  if (opt->dbg_regalloc_heat)
    return "--regalloc-heat";
  if (opt->dbg_elf_dump)
    return "--elf-dump";
  if (opt->callgraph)
    return "--callgraph";
  if (opt->native_tier_report)
    return "--native-tier-report";
  if (opt->stop_after != NY_STOP_AFTER_NONE || opt->stop_after_raw)
    return "--stop-after";
  if (opt->dbg_abi)
    return "--abi";
  if (opt->dump_ast || opt->dump_tokens || opt->dump_on_error ||
      opt->dump_diagnose || opt->dump_escapes || opt->dump_proofs ||
      opt->dump_llvm_passes || opt->native_dump_ir || opt->nyr_run ||
      opt->nyr_metadata_report || opt->nyr_ir_verify || opt->nyr_bisect ||
      opt->nyr_fingerprint || opt->nyr_flaky_check || opt->trace_tokens ||
      opt->trace_parse || opt->trace_scope || opt->trace_hm ||
      opt->trace_llvm || opt->trace_regalloc || opt->explain ||
      opt->explain_any || opt->compile_profile_fn)
    return "inspection option";
  if (opt->emit_wasm_path || opt->emit_module)
    return "--emit-wasm";
  return NULL;
}

static int __attribute__((unused)) ny_try_bare_aot_command(const ny_options *opt, ny_tick_t t_start) {
  if (!opt || opt->runtime_mode != NY_RUNTIME_MODE_BARE ||
      opt->bare_native_fallback)
    return -1;
  const char *unsupported = ny_bare_aot_unsupported_inspection(opt);
  if (unsupported) {
    fprintf(stderr, "ny: %s is not supported with --mode=bare AOT\n",
            unsupported);
    return 2;
  }
  if (!opt->run_aot && !opt->emit_only)
    return -1;
#if defined(__linux__) && (defined(__x86_64__) || defined(__aarch64__))
  if ((!opt->command_string && !opt->input_file) || !opt->output_file ||
      (opt->native_backend_explicit &&
       opt->native_backend != NY_NATIVE_BACKEND_LLVM &&
       opt->native_backend != NY_NATIVE_BACKEND_X86_64 &&
       opt->native_backend != NY_NATIVE_BACKEND_AARCH64)) {
    fprintf(stderr, "bare AOT supports Linux x86-64 and dependency-free Linux "
                    "AArch64 NyR executables; this target or output path is "
                    "unsupported\n");
    return 1;
  }
  char *file_source = NULL;
  const char *source = opt->command_string;
  if (!source && opt->input_file) {
    file_source = ny_read_file(opt->input_file);
    source = file_source;
  }
  ny_tiny_program_t program;
  if (!source || !ny_parse_tiny_program(source, &program)) {
    bool needs_native = ny_bare_source_needs_native(source);
    free(file_source);
    if (needs_native)
      return -2;
    fprintf(stderr, "bare AOT could not parse a constant-output program or "
                    "find a native function to compile\n");
    return 1;
  }
  free(file_source);
  bool ok = ny_compile_tiny_program(opt, &program);
  ny_tiny_program_free(&program);
  if (!ok) {
    fprintf(stderr, "bare AOT could not emit a freestanding executable\n");
    return 1;
  }
  ny_log_saved_native_artifact(opt->output_file);
  ny_tiny_timing_report(opt, t_start, true);
  return 0;
#else
  (void)t_start;
  fprintf(stderr,
          "bare AOT currently requires a Linux x86-64 or AArch64 host\n");
  return 1;
#endif
}

static bool ny_try_fast_command_string(const ny_options *opt,
                                       ny_tick_t t_start) {
  if (!opt || !opt->command_string || opt->input_file || opt->run_aot ||
      opt->expand || opt->mode != NY_MODE_RUN || opt->dump_on_error ||
      opt->dump_diagnose || opt->dump_ast || opt->dump_tokens ||
      opt->dump_llvm || opt->dump_funcs || opt->dump_escapes ||
      opt->dump_proofs || opt->dump_llvm_passes || opt->dump_obj_full ||
      opt->trace_tokens || opt->trace_parse || opt->trace_scope ||
      opt->trace_hm || opt->trace_llvm || opt->trace_regalloc || opt->explain ||
      opt->explain_any || opt->compile_profile_fn || opt->nyr_ir_verify ||
      opt->nyr_bisect || opt->nyr_fingerprint || opt->nyr_flaky_check ||
      opt->nyr_track_value >= 0 || opt->dbg_filter || opt->dbg_delta_only ||
      opt->dbg_unify_trace || opt->dbg_trait_resolve || opt->dbg_format_json ||
      opt->dbg_abi || opt->dbg_elf_dump || opt->dbg_regalloc_heat ||
      opt->dbg_stack_layout || opt->native_result_oracle ||
      opt->native_oracle_per_pass || opt->native_tier_report ||
      opt->callgraph || opt->emit_ir_path || opt->emit_bc_path ||
      opt->emit_asm_path || opt->emit_wasm_path || opt->emit_report_path ||
      opt->emit_artifact_path || opt->emit_shapes ||
      opt->emit_refinement_interface_path || opt->emit_effect_interface_path ||
      opt->native_profile_use_path || opt->collect_errors || opt->verbosity > 0)
    return false;
  if (opt->emit_only && !opt->output_file)
    return false;
  if (opt->native_backend != NY_NATIVE_BACKEND_LLVM || opt->native_dump_ir ||
      opt->nyr_run || opt->nyr_metadata_report)
    return false;

  ny_tiny_command_t cmd;
  if (!ny_parse_tiny_command_string(opt->command_string, &cmd))
    return false;

  if (opt->output_file) {
    bool ok = ny_compile_tiny_command(opt, &cmd);
    ny_tiny_command_free(&cmd);
    if (!ok)
      return false;
    ny_log_saved_native_artifact(opt->output_file);
    ny_tiny_timing_report(opt, t_start, true);
    return true;
  }

  if (cmd.kind == NY_TINY_CMD_PRINT || cmd.kind == NY_TINY_CMD_EPRINT) {
    FILE *out = cmd.kind == NY_TINY_CMD_EPRINT ? stderr : stdout;
    fputs(cmd.text, out);
    fputc('\n', out);
    fflush(out);
  }
  ny_tiny_command_free(&cmd);
  ny_tiny_timing_report(opt, t_start, false);
  return true;
}

static char *ny_read_stdin_all(void) {
  size_t cap = 4096;
  size_t len = 0;
  char *buf = malloc(cap);
  if (!buf)
    return NULL;
  int ch;
  while ((ch = fgetc(stdin)) != EOF) {
    if (len + 1 >= cap) {
      if (cap > SIZE_MAX / 2) {
        free(buf);
        return NULL;
      }
      size_t next_cap = cap * 2;
      char *next = realloc(buf, next_cap);
      if (!next) {
        free(buf);
        return NULL;
      }
      buf = next;
      cap = next_cap;
    }
    buf[len++] = (char)ch;
  }
  buf[len] = '\0';
  return buf;
}

static bool ny_repl_batch_can_fast_run(const char *src) {
  if (!src)
    return false;
  if (!ny_env_enabled("NYTRIX_REPL_FAST_PIPE"))
    return false;
  const char *p = src;
  while (*p) {
    while (*p && isspace((unsigned char)*p))
      p++;
    if (*p == '\0')
      return true;
    if (*p == ';') {
      while (*p && *p != '\n')
        p++;
      continue;
    }
    if (*p == '#') {
      if (p[1] == '!') {
        while (*p && *p != '\n')
          p++;
        continue;
      }
      while (*p && *p != '\n')
        p++;
      continue;
    }
    if (*p == ':')
      return false;
    if (strncmp(p, "module", 6) == 0 &&
        (p[6] == '\0' || isspace((unsigned char)p[6]) || p[6] == '('))
      return false;
    return true;
  }
  return true;
}

static bool ny_valid_native_artifact(const char *path) {
  if (!path || !*path)
    return false;
  struct stat st;
  if (stat(path, &st) != 0)
    return false;
  if (st.st_size <= 0)
    return false;
#ifndef _WIN32
  if (ny_access(path, X_OK) != 0)
    return false;
#endif
  FILE *f = fopen(path, "rb");
  if (!f)
    return false;
  unsigned char hdr[4] = {0, 0, 0, 0};
  size_t n = fread(hdr, 1, sizeof(hdr), f);
  fclose(f);
  if (n < 2)
    return false;
#ifdef _WIN32
  return hdr[0] == 'M' && hdr[1] == 'Z';
#else
  return n >= 4 && hdr[0] == 0x7f && hdr[1] == 'E' && hdr[2] == 'L' &&
         hdr[3] == 'F';
#endif
}
