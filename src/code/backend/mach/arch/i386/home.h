/* Promote frequently accessed, non-addressable spill homes to nonvolatile
 * registers across basic-block boundaries. Keep the frame's alignment by
 * moving all private homes down 16 bytes and saving ESI/EDI above them. */
typedef struct {
  size_t uses;
  bool banned;
} sm_i386_home_t;

static bool sm_i386_home_opcode(const char *line) {
  static const char *const names[] = {"movl",  "addl",  "subl", "imull",
                                      "andl",  "orl",   "xorl", "cmpl",
                                      "testl", "pushl", "popl"};
  if (*line != '\t')
    return false;
  ++line;
  for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
    size_t n = strlen(names[i]);
    if (!strncmp(line, names[i], n) && (line[n] == '\t' || line[n] == ' '))
      return true;
  }
  return false;
}

static const char *sm_i386_home_number(const char *line, const char *operand) {
  const char *p = operand;
  while (p > line && p[-1] >= '0' && p[-1] <= '9')
    --p;
  if (p > line && p[-1] == '-')
    --p;
  return p;
}

static bool sm_i386_promote_line(nyr_writer_t *out, const char *line,
                                 const char *end, const int homes[3],
                                 const unsigned char *colors, size_t count,
                                 int frame_shift) {
  const char *p = line;
  while (p < end) {
    const char *operand = strstr(p, "(%ebp)");
    if (!operand || operand >= end)
      break;
    const char *number = sm_i386_home_number(p, operand);
    char *parsed = NULL;
    long offset = strtol(number, &parsed, 10);
    if (number == operand || parsed != operand) {
      if (!nyr_printf(out, "%.*s", (int)(operand + 6 - p), p))
        return false;
      p = operand + 6;
      continue;
    }
    if (!nyr_printf(out, "%.*s", (int)(number - p), p))
      return false;
    unsigned color = colors && offset <= -8 && offset % 4 == 0 &&
                             (uint64_t)(-offset - 8) / 4 < count
                         ? colors[(size_t)(-offset - 8) / 4]
                     : homes && offset == homes[0] ? 1
                     : homes && offset == homes[1] ? 2
                     : homes && offset == homes[2] ? 3
                                                   : 0;
    if (color) {
      const char *reg = color == 1 ? "%esi" : color == 2 ? "%edi" : "%ebx";
      if (!nyr_put(out, reg))
        return false;
    } else if (offset < 0) {
      if (!nyr_printf(out, "%ld(%%ebp)", offset - frame_shift))
        return false;
    } else if (!nyr_printf(out, "%.*s", (int)(operand + 6 - number), number)) {
      return false;
    }
    p = operand + 6;
  }
  return nyr_printf(out, "%.*s\n", (int)(end - p), p);
}

static bool sm_i386_preallocated_home_layout(const char *assembly,
                                             const char **body,
                                             const char **epilogue,
                                             bool *uses_ebx) {
  static const char prologue[] =
      "\tpushl\t%ebp\n\tmovl\t%esp, %ebp\n\tpushl\t%ebx\n"
      "\tpushl\t%esi\n\tpushl\t%edi\n\tsubl\t$8, %esp\n";
  static const char finish[] =
      "\tleal\t-12(%ebp), %esp\n\tpopl\t%edi\n\tpopl\t%esi\n"
      "\tpopl\t%ebx\n\tpopl\t%ebp\n";
  const char *start = assembly ? strstr(assembly, prologue) : NULL;
  if (!start)
    return false;
  start += sizeof(prologue) - 1;
  const char *end = strstr(start, finish);
  if (!end)
    return false;
  bool ebx = false;
  for (const char *line = start; line < end;) {
    const char *line_end = memchr(line, '\n', (size_t)(end - line));
    if (!line_end)
      line_end = end;
    size_t length = (size_t)(line_end - line);
    if (ny_memmem(line, length, "%esi", 4) ||
        ny_memmem(line, length, "%edi", 4))
      return false;
    ebx |= ny_memmem(line, length, "%ebx", 4) != NULL;
    line = line_end < end ? line_end + 1 : end;
  }
  if (body)
    *body = start;
  if (epilogue)
    *epilogue = end;
  if (uses_ebx)
    *uses_ebx = ebx;
  return true;
}

/* The register-run emitter saves EBX in some variants even when its body
 * leaves EBX unused. Such a saved register can hold another private home. */
static bool sm_i386_saved_ebx_unused(const char *assembly) {
  static const char prologue[] =
      "\tpushl\t%ebp\n\tmovl\t%esp, %ebp\n\tpushl\t%ebx\n";
  const char *start = assembly ? strstr(assembly, prologue) : NULL;
  if (!start)
    return false;
  start += sizeof(prologue) - 1;
  const char *end = strstr(start, "\tleave\n");
  const char *candidate = strstr(start, "\tleal\t-4(%ebp), %esp\n");
  if (candidate && (!end || candidate < end))
    end = candidate;
  if (!end)
    return false;
  return !ny_memmem(start, (size_t)(end - start), "%ebx", 4);
}

static int sm_i386_write_promoted_homes(const char *assembly, nyr_writer_t *out,
                                        const int homes[3],
                                        const unsigned char *colors,
                                        size_t count) {
  const char *global_body = NULL, *global_epilogue = NULL;
  bool global_uses_ebx = false;
  bool preallocated = sm_i386_preallocated_home_layout(
      assembly, &global_body, &global_epilogue, &global_uses_ebx);
  bool colored_ebx = false;
  if (colors)
    for (size_t i = 0; i < count; ++i)
      colored_ebx |= colors[i] == 3;
  if (colored_ebx && !preallocated && !sm_i386_saved_ebx_unused(assembly))
    return 0;
  if (homes && homes[2] != INT_MIN && (!preallocated || global_uses_ebx))
    return 0;
  const char *prologue =
      strstr(assembly, "\tpushl\t%ebp\n\tmovl\t%esp, %ebp\n");
  if (!prologue)
    return 0;
  const char *insert = prologue + strlen("\tpushl\t%ebp\n\tmovl\t%esp, %ebp\n");
  bool ebx = !strncmp(insert, "\tpushl\t%ebx\n", strlen("\tpushl\t%ebx\n"));
  if (ebx)
    insert += strlen("\tpushl\t%ebx\n");
  if (preallocated)
    insert = global_body;
  const char *epilogue =
      preallocated
          ? "\tleal\t-12(%ebp), "
            "%esp\n\tpopl\t%edi\n\tpopl\t%esi\n\tpopl\t%ebx\n\tpopl\t%ebp\n"
      : colored_ebx ? "\tleal\t-4(%ebp), %esp\n\tpopl\t%ebx\n\tpopl\t%ebp\n"
      : ebx         ? "\tleal\t-4(%ebp), %esp\n\tpopl\t%ebx\n\tpopl\t%ebp\n"
                    : "\tleave\n";
  if (!strstr(assembly, epilogue))
    return 0;
  bool ok = true;
  for (const char *line = assembly; ok && *line;) {
    if (line == insert && !preallocated)
      ok = nyr_put(out, "\tpushl\t%esi\n\tpushl\t%edi\n\tsubl\t$8, %esp\n");
    if (!strncmp(line, epilogue, strlen(epilogue))) {
      if (preallocated) {
        ok = ok && nyr_put(out, epilogue);
        line += strlen(epilogue);
        continue;
      }
      ok = ok &&
           nyr_put(out,
                   (ebx || colored_ebx)
                       ? "\tleal\t-12(%ebp), "
                         "%esp\n\tpopl\t%edi\n\tpopl\t%esi\n\tpopl\t%"
                         "ebx\n\tpopl\t%ebp\n"
                       : "\tleal\t-8(%ebp), "
                         "%esp\n\tpopl\t%edi\n\tpopl\t%esi\n\tpopl\t%ebp\n");
      line += strlen(epilogue);
      continue;
    }
    const char *end = strchr(line, '\n');
    if (!end)
      end = line + strlen(line);
    nyr_writer_t rewritten = {0};
    ok = ok && sm_i386_promote_line(&rewritten, line, end, homes, colors, count,
                                    preallocated ? 0 : 16);
    if (ok && strcmp(rewritten.data, "\tmovl\t%esi, %esi\n") &&
        strcmp(rewritten.data, "\tmovl\t%edi, %edi\n"))
      ok = nyr_put(out, rewritten.data);
    free(rewritten.data);
    line = *end ? end + 1 : end;
  }
  return ok ? 1 : -1;
}

static int sm_i386_promote_homes(const nyr_func_t *f, const int *offsets,
                                 const unsigned char *high_values,
                                 const char *assembly, nyr_writer_t *out) {
  const char *global_body = NULL, *global_epilogue = NULL;
  bool body_uses_ebx = false;
  bool preallocated = sm_i386_preallocated_home_layout(
      assembly, &global_body, &global_epilogue, &body_uses_ebx);
  if (!f || !assembly || !offsets || !high_values ||
      (!preallocated &&
       (strstr(assembly, "%esi") || strstr(assembly, "%edi"))) ||
      strstr(assembly, ".cfi_"))
    return 0;
  bool addressed = false;
  size_t locals = f->param_count;
  int bytes = 0;
  for (size_t v = 0; v < (size_t)f->next_value; ++v) {
    int end = offsets[v] + (high_values[v] ? 8 : 4);
    if (end > bytes)
      bytes = end;
  }
  for (size_t i = 0; i < f->len; ++i) {
    const nyr_inst_t *in = &f->data[i];
    if (in->op == NYIR_INLINE_ASM &&
        (in->imm > 0 || in->imm == -194 ||
         (!sm_i386_asm_low_operands(in) && in->imm != -21 && in->imm != -22 &&
          in->imm != -23)))
      return 0;
    addressed |= in->op == NYIR_ADDR_LOCAL;
    if ((in->op == NYIR_LOAD_LOCAL || in->op == NYIR_STORE_LOCAL ||
         in->op == NYIR_ADDR_LOCAL) &&
        in->imm >= 0 && in->imm < INT_MAX && (uint64_t)in->imm >= locals)
      locals = (size_t)in->imm + 1;
  }
  if (!addressed) {
    if (locals > (size_t)(INT_MAX - bytes - 32) / 8)
      return 0;
    bytes += (int)locals * 8;
  }
  if (bytes > INT_MAX - 32)
    return 0;
  int minimum = -4 - bytes;
  if (preallocated)
    minimum -= 16;
  size_t count = (size_t)(-minimum) / 4 + 1;
  sm_i386_home_t *slots = calloc(count, sizeof(*slots));
  if (!slots)
    return -1;
  for (const char *line = assembly; *line;) {
    const char *end = strchr(line, '\n');
    if (!end)
      end = line + strlen(line);
    bool allowed = sm_i386_home_opcode(line);
    for (const char *p = line; p < end;) {
      const char *operand = strstr(p, "(%ebp)");
      if (!operand || operand >= end)
        break;
      const char *number = sm_i386_home_number(line, operand);
      char *parsed = NULL;
      long offset = strtol(number, &parsed, 10);
      if (number != operand && parsed == operand && offset >= minimum &&
          offset <= -8) {
        if (allowed && offset % 4 == 0) {
          ++slots[(size_t)(-offset) / 4].uses;
        } else {
          /* Wider/subword and address-forming accesses cannot become a GPR.
           * Conservatively exclude neighboring words of a wider access too. */
          for (size_t s = 2; s < count; ++s) {
            long slot = -(long)s * 4;
            if (slot < offset + 32 && offset < slot + 4)
              slots[s].banned = true;
          }
        }
      }
      p = operand + 6;
    }
    line = *end ? end + 1 : end;
  }
  int homes[3] = {INT_MIN, INT_MIN, INT_MIN};
  size_t best[3] = {0, 0, 0};
  size_t registers = preallocated && !body_uses_ebx ? 3 : 2;
  for (size_t s = 2; s < count; ++s) {
    if (slots[s].banned)
      continue;
    if (slots[s].uses > best[0]) {
      if (registers > 2) {
        best[2] = best[1];
        homes[2] = homes[1];
      }
      best[1] = best[0];
      homes[1] = homes[0];
      best[0] = slots[s].uses;
      homes[0] = -(int)s * 4;
    } else if (slots[s].uses > best[1]) {
      if (registers > 2) {
        best[2] = best[1];
        homes[2] = homes[1];
      }
      best[1] = slots[s].uses;
      homes[1] = -(int)s * 4;
    } else if (registers > 2 && slots[s].uses > best[2]) {
      best[2] = slots[s].uses;
      homes[2] = -(int)s * 4;
    }
  }
  free(slots);
  if (best[0] < 8)
    return 0;
  return sm_i386_write_promoted_homes(assembly, out, homes, NULL, 0);
}
