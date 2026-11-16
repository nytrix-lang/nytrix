/* Final cleanup of the stack emitter's private value homes and non-addressed
 * source locals. Incoming arguments and fixed alloca storage never participate.
 * Memory liveness follows assembly branches, including
 * loops; unresolved/indirect branches conservatively keep every home live. */
typedef struct {
  char *text;
  char op[24];
  char *args;
  int read[8], reads, write;
  size_t target;
  bool all, terminal, conditional, removed, label, incoming;
  unsigned char reg_read, reg_write, reg_live;
  bool reg_removable;
  char *replacement;
} sm_i386_spill_line_t;

typedef struct { const char *name; size_t line; } sm_i386_spill_label_t;

typedef struct { size_t slot, weight; } sm_i386_home_rank_t;

static int sm_i386_home_rank_order(const void *a, const void *b) {
  const sm_i386_home_rank_t *x = a, *y = b;
  if (x->weight != y->weight) return x->weight > y->weight ? -1 : 1;
  return x->slot < y->slot ? -1 : x->slot != y->slot;
}

/* Permute private word homes only. The alias exclusions computed below keep
 * address-taken objects and multiword memory operands at their original
 * offsets. No homes are merged and the stack frame retains its size. */
static bool sm_i386_write_ranked_homes(nyr_writer_t *out, const char *text,
                                       const size_t *map, size_t slots) {
  const char *p = text, *operand;
  while ((operand = strstr(p, "(%ebp)"))) {
    const char *number = sm_i386_home_number(p, operand);
    char *end = NULL;
    long offset = strtol(number, &end, 10);
    size_t slot = offset <= -8 && offset % 4 == 0
                      ? (size_t)((-offset - 8) / 4) : slots;
    if (number != operand && end == operand && slot < slots) {
      if (!nyr_printf(out, "%.*s%ld(%%ebp)", (int)(number - p), p,
                       -8L - (long)map[slot] * 4)) return false;
      p = operand + 6;
    } else {
      if (!nyr_printf(out, "%.*s", (int)(operand + 6 - p), p)) return false;
      p = operand + 6;
    }
  }
  return nyr_put(out, p);
}

static int sm_i386_spill_label_order(const void *a, const void *b) {
  return strcmp(((const sm_i386_spill_label_t *)a)->name,
                ((const sm_i386_spill_label_t *)b)->name);
}

static int sm_i386_spill_register(const char *s) {
  static const char *const names[8][4] = {
      {"%eax", "%ax", "%al", "%ah"}, {"%ecx", "%cx", "%cl", "%ch"},
      {"%edx", "%dx", "%dl", "%dh"}, {"%ebx", "%bx", "%bl", "%bh"},
      {"%esi", "%si", NULL, NULL}, {"%edi", "%di", NULL, NULL},
      {"%ebp", "%bp", NULL, NULL}, {"%esp", "%sp", NULL, NULL}};
  for (int r = 0; r < 8; ++r)
    for (int k = 0; k < 4; ++k)
      if (names[r][k] && !strcmp(s, names[r][k])) return r;
  return -1;
}

static int sm_i386_spill_slot(const char *operand, int bytes) {
  char *end = NULL;
  long offset = strtol(operand, &end, 10);
  if (end == operand || strcmp(end, "(%ebp)") || offset % 4 ||
      offset > -8 || offset < -4L - bytes) return -1;
  return (int)((-offset - 8) / 4);
}

static unsigned sm_i386_spill_register_mask(const char *s) {
  unsigned mask = 0;
  for (const char *p = s; p && (p = strchr(p, '%'));) {
    char name[12]; size_t n = 1;
    while ((p[n] >= 'a' && p[n] <= 'z') || (p[n] >= '0' && p[n] <= '9')) ++n;
    if (n >= sizeof(name)) return 255;
    memcpy(name, p, n); name[n] = 0;
    int reg = sm_i386_spill_register(name);
    if (reg >= 0) mask |= 1u << reg;
    p += n;
  }
  return mask;
}

static bool sm_i386_spill_split(const sm_i386_spill_line_t *in,
                                  char a[192], char b[192]) {
  if (!in->args || strlen(in->args) >= 190) return false;
  const char *comma = strchr(in->args, ',');
  if (!comma || strchr(comma + 1, ',')) return false;
  size_t n = (size_t)(comma - in->args);
  while (n && (in->args[n - 1] == ' ' || in->args[n - 1] == '\t')) --n;
  memcpy(a, in->args, n); a[n] = 0;
  ++comma;
  while (*comma == ' ' || *comma == '\t') ++comma;
  strcpy(b, comma);
  return true;
}

static const char *sm_i386_spill_inverse(const char *op) {
  static const char *const pairs[][2] = {
      {"je", "jne"}, {"jz", "jnz"}, {"jl", "jge"}, {"jle", "jg"},
      {"jb", "jae"}, {"jbe", "ja"}, {"js", "jns"}, {"jo", "jno"},
      {"jp", "jnp"}};
  for (size_t i = 0; i < sizeof(pairs) / sizeof(pairs[0]); ++i)
    for (int k = 0; k < 2; ++k)
      if (!strcmp(op, pairs[i][k])) return pairs[i][1 - k];
  return NULL;
}

static const char *sm_i386_setcc_branch(const char *op) {
  static const char *const pairs[][2] = {
      {"sete", "je"}, {"setz", "je"}, {"setne", "jne"},
      {"setnz", "jne"}, {"setl", "jl"}, {"setle", "jle"},
      {"setg", "jg"}, {"setge", "jge"}, {"setb", "jb"},
      {"setc", "jb"}, {"setnae", "jb"}, {"setbe", "jbe"},
      {"setna", "jbe"}, {"seta", "ja"}, {"setnbe", "ja"},
      {"setae", "jae"}, {"setnb", "jae"}, {"setnc", "jae"},
      {"sets", "js"}, {"setns", "jns"}, {"seto", "jo"},
      {"setno", "jno"}, {"setp", "jp"}, {"setpe", "jp"},
      {"setnp", "jnp"}, {"setpo", "jnp"}};
  for (size_t i = 0; i < sizeof(pairs) / sizeof(pairs[0]); ++i)
    if (!strcmp(op, pairs[i][0])) return pairs[i][1];
  return NULL;
}

static bool sm_i386_has_live_label_incoming(const sm_i386_spill_line_t *lines,
                                            size_t count, size_t label) {
  for (size_t i = 0; i < count; ++i)
    if (!lines[i].removed && lines[i].target == label)
      return true;
  return false;
}

/* Recompute the per-line register liveness after a pass edits the stream. */
static void sm_i386_spill_reg_live(sm_i386_spill_line_t *lines, size_t count) {
  bool changed;
  do {
    changed = false;
    for (size_t at = count; at-- > 0;) {
      sm_i386_spill_line_t *in = &lines[at];
      unsigned out = (!in->terminal || in->removed) && at + 1 < count
                         ? lines[at + 1].reg_live : 0;
      if (!in->removed && in->target < count) out |= lines[in->target].reg_live;
      unsigned before = in->removed ? out :
          (out & ~(unsigned)in->reg_write) | in->reg_read;
      if (before != in->reg_live) {
        in->reg_live = (unsigned char)before; changed = true;
      }
    }
  } while (changed);
}

/* Instructions that read the current flags without setting them first. */
static bool sm_i386_flag_consumer(const char *op) {
  if (op[0] == 'j' && strcmp(op, "jmp") && strcmp(op, "jmpq") &&
      strcmp(op, "jcxz") && strcmp(op, "jecxz"))
    return true;
  return !strncmp(op, "set", 3) || !strncmp(op, "cmov", 4) ||
         !strcmp(op, "adcl") || !strcmp(op, "sbbl") ||
         !strcmp(op, "adcw") || !strcmp(op, "sbbw") ||
         !strcmp(op, "adcb") || !strcmp(op, "sbbb") || !strcmp(op, "cmc");
}

/* Instructions that overwrite every flag a discarded SARL could set. */
static bool sm_i386_flag_writer(const char *op) {
  static const char *const names[] = {
      "addl", "subl", "andl", "orl", "xorl", "cmpl", "testl", "incl",
      "decl", "negl", "imull", "shll", "shrl", "sarl", "addw", "subw",
      "andw", "orw", "xorw", "cmpw", "testw", "incw", "decw", "negw",
      "imulw", "shlw", "shrw", "sarw", "addb", "subb", "andb", "orb",
      "xorb", "cmpb", "testb", "incb", "decb", "negb", "imulb", "shlb",
      "shrb", "sarb"};
  for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i)
    if (!strcmp(op, names[i])) return true;
  return !strncmp(op, "call", 4) || !strncmp(op, "ret", 3) ||
         !strcmp(op, "ud2");
}

static bool sm_i386_direct_call_reads(const nyr_func_t *func,
                                      const char *assembly_target,
                                      size_t *cursor, unsigned *reads) {
  if (!func || !assembly_target || !cursor || !reads ||
      assembly_target[0] == '*')
    return false;
  size_t name_len = strcspn(assembly_target, " \t");
  if (!name_len) return false;
  for (size_t i = *cursor; i < func->len; ++i) {
    const nyr_inst_t *call = &func->data[i];
    if (call->op != NYIR_CALL || !call->symbol ||
        strlen(call->symbol) != name_len ||
        memcmp(call->symbol, assembly_target, name_len))
      continue;
    *cursor = i + 1;
    if (call->flags & (NYIR_INST_F_INDIRECT | NYIR_INST_F_SRET))
      return false;
    int args[NYIR_CALL_MAX_ARGS];
    int argc = 0;
    if (!nyr_call_args(call, func->next_value, args, NYIR_CALL_MAX_ARGS,
                       &argc, NULL, 0))
      return false;
    unsigned read = 0;
    unsigned regparm = (call->flags & NYIR_INST_F_I386_REGPARM_MASK) >>
                       NYIR_INST_F_I386_REGPARM_SHIFT;
    unsigned used = 0;
    static const unsigned reg_bits[] = {1u << 0, 1u << 2, 1u << 1};
    for (int a = 0; a < argc; ++a) {
      nyr_scalar_class_t cls = nyr_call_arg_class_get(call, (size_t)a);
      size_t size = cls == NYIR_SCALAR_AGGREGATE
                        ? NYIR_ARG_AGG_SIZE(call->arg_sizes[a])
                    : cls == NYIR_SCALAR_F64 ||
                              (call->i386_wide_args & (UINT64_C(1) << a))
                        ? 8u : 4u;
      bool floating = cls == NYIR_SCALAR_F32 || cls == NYIR_SCALAR_F64 ||
                      (cls == NYIR_SCALAR_AGGREGATE &&
                       NYIR_ARG_AGG_CLASS(call->arg_sizes[a], 0) ==
                           NYIR_ARG_CLASS_SSE);
      int reg = sm_i386_argument_register(&used, regparm, floating, size);
      if (reg < 0) continue;
      for (size_t piece = 0; piece * 4u < size && (size_t)reg + piece < 3;
           ++piece)
        read |= reg_bits[(size_t)reg + piece];
    }
    if (call->flags & NYIR_INST_F_VARIADIC)
      read |= 1u;
    *reads = read;
    return true;
  }
  return false;
}

static int sm_i386_color_spill_homes(const nyr_func_t *func,
                                       const char *assembly, nyr_writer_t *out,
                                       const sm_i386_spill_line_t *lines,
                                       size_t count, size_t slots, size_t words,
                                       const unsigned char *banned,
                                       const uint64_t *live) {
  if (slots > 4096 || strstr(assembly, "%esi") || strstr(assembly, "%edi") ||
      strstr(assembly, ".cfi_") ||
      getenv("NYC_DISABLE_I386_HOME_COLORING")) return 0;
  bool preallocated_ebx_used = false;
  bool preallocated = sm_i386_preallocated_home_layout(
      assembly, NULL, NULL, &preallocated_ebx_used);
  size_t register_count = preallocated
                              ? (!preallocated_ebx_used ? 3u : 2u)
                              : (sm_i386_saved_ebx_unused(assembly) ? 3u : 2u);
  for (size_t i = 0; i < func->len; ++i) {
    const nyr_inst_t *in = &func->data[i];
    if (in->op == NYIR_INLINE_ASM &&
        (in->imm > 0 || in->imm == -194 ||
         (!sm_i386_asm_low_operands(in) && in->imm != -21 &&
          in->imm != -22 && in->imm != -23))) return 0;
  }
  size_t *weight = calloc(slots, sizeof(*weight));
  unsigned char *colors = calloc(slots, 1);
  uint64_t *graph = calloc(slots * words, sizeof(*graph));
  uint64_t *occupied = calloc(words * register_count, sizeof(*occupied));
  if (!weight || !colors || !graph || !occupied) {
    free(weight); free(colors); free(graph); free(occupied); return -1;
  }
  for (size_t i = 0; i < count; ++i) {
    if (lines[i].removed) continue;
    for (int k = 0; k < lines[i].reads; ++k) ++weight[lines[i].read[k]];
    if (lines[i].write >= 0) ++weight[lines[i].write];
    for (size_t s = 0; s < slots; ++s) {
      if (banned[s] || !(live[i * words + s / 64] & (UINT64_C(1) << (s % 64)))) continue;
      for (size_t w = 0; w < words; ++w) graph[s * words + w] |= live[i * words + w];
    }
    int dst = lines[i].write;
    if (dst >= 0 && !banned[dst] && i + 1 < count) {
      for (size_t s = 0; s < slots; ++s) {
        if (!(live[(i + 1) * words + s / 64] & (UINT64_C(1) << (s % 64)))) continue;
        graph[(size_t)dst * words + s / 64] |= UINT64_C(1) << (s % 64);
        graph[s * words + (size_t)dst / 64] |= UINT64_C(1) << (dst % 64);
      }
    }
  }
  size_t selected = 0;
  for (;;) {
    size_t best = slots, uses = 1;
    uint64_t best_score = 0;
    for (size_t s = 0; s < slots; ++s) {
      if (banned[s] || colors[s] || weight[s] <= 1) continue;
      size_t degree = 0;
      for (size_t w = 0; w < words; ++w)
        degree += (size_t)__builtin_popcountll(graph[s * words + w]);
      uint64_t divisor = (uint64_t)(degree + 1u) * (degree + 1u);
      uint64_t score = ((uint64_t)weight[s] << 16) / divisor;
      if (score > best_score || (score == best_score && weight[s] > uses)) {
        best_score = score;
        uses = weight[s];
        best = s;
      }
    }
    if (best == slots) break;
    unsigned color = 0;
    uint64_t bit = UINT64_C(1) << (best % 64);
    for (unsigned candidate = 0; candidate < register_count; ++candidate)
      if (!(occupied[candidate * words + best / 64] & bit)) {
        color = candidate + 1;
        break;
      }
    if (color) {
      colors[best] = (unsigned char)color;
      for (size_t w = 0; w < words; ++w)
        occupied[(color - 1u) * words + w] |= graph[best * words + w];
      ++selected;
    }
    weight[best] = 0;
  }
  int result = selected ? sm_i386_write_promoted_homes(assembly, out, NULL, colors, slots) : 0;
  free(weight); free(colors); free(graph); free(occupied);
  return result;
}

static bool sm_i386_cleanup_spills(nyr_writer_t *assembly, int value_bytes,
                                     const int *local_offsets, size_t locals,
                                     int local_bytes, const nyr_func_t *func,
                                     nyr_writer_t *colored) {
  if (local_bytes < 0 || local_bytes > INT_MAX - value_bytes) return true;
  int bytes = value_bytes + local_bytes;
  if (!assembly->data || bytes <= 0 || getenv("NYC_DISABLE_I386_SPILL_CLEANUP"))
    return true;
  size_t count = 0;
  for (const char *p = assembly->data; *p; ++p) count += *p == '\n';
  if (!count || count > 100000) return true;
  size_t slots = (size_t)bytes / 4, words = (slots + 63) / 64;
  if (!words || count > (64u * 1024u * 1024u) / sizeof(uint64_t) / words)
    return true;
  char *copy = strdup(assembly->data);
  sm_i386_spill_line_t *lines = calloc(count, sizeof(*lines));
  sm_i386_spill_label_t *labels = calloc(count, sizeof(*labels));
  unsigned char *banned = calloc(slots, 1);
  uint64_t *live = calloc(count * words, sizeof(*live));
  uint64_t *scratch = calloc(words, sizeof(*scratch));
  bool ok = copy && lines && labels && banned && live && scratch;
  size_t label_count = 0;
  if (!ok) goto done;
  for (size_t local = 0; local < locals; ++local) {
    bool addressed = false;
    for (size_t i = 0; !addressed && i < func->len; ++i) {
      const nyr_inst_t *in = &func->data[i];
      addressed = (in->op == NYIR_ADDR_LOCAL && in->imm == (int64_t)local) ||
                  ((in->op == NYIR_LOAD_LOCAL || in->op == NYIR_STORE_LOCAL) &&
                   in->imm == (int64_t)local &&
                   ((in->flags & NYIR_INST_F_MEM_VOLATILE) ||
                    (in->effects & (NYIR_EFFECT_VOLATILE | NYIR_EFFECT_IO)))) ||
                  (in->op == NYIR_INLINE_ASM &&
                   sm_i386_asm_touches_local(in, (int)local));
    }
    if (addressed) {
      if (!local_offsets || local >= locals) {
        memset(banned, 1, slots);
        continue;
      }
      size_t index = (size_t)(value_bytes + local_offsets[local] - 4) / 4;
      if (index >= slots) {
        memset(banned, 1, slots);
        continue;
      }
      banned[index] = 1;
      if (local_offsets[local] - (local ? local_offsets[local - 1] : 0) == 8 &&
          index + 1 < slots)
        banned[index + 1] = 1;
    }
  }
  char *p = copy;
  bool unknown_branch = false;
  for (size_t i = 0; i < count; ++i) {
    sm_i386_spill_line_t *in = &lines[i];
    in->text = p; in->write = -1; in->target = SIZE_MAX;
    char *end = strchr(p, '\n');
    if (end) *end = 0;
    p = end ? end + 1 : p + strlen(p);
    size_t len = strlen(in->text);
    if (len && in->text[len - 1] == ':' && in->text[0] != '\t') {
      in->text[len - 1] = 0;
      in->label = true;
      labels[label_count++] = (sm_i386_spill_label_t){in->text, i};
      continue;
    }
    if (in->text[0] != '\t') continue;
    char *op = in->text + 1;
    size_t n = strcspn(op, " \t");
    if (n >= sizeof(in->op)) continue;
    memcpy(in->op, op, n); in->op[n] = 0;
    op += n;
    while (*op == ' ' || *op == '\t') ++op;
    in->args = op;
  }
  qsort(labels, label_count, sizeof(*labels), sm_i386_spill_label_order);
  for (size_t i = 0; i < count; ++i) {
    sm_i386_spill_line_t *in = &lines[i];
    if (in->op[0] == 'j' && in->args) {
      sm_i386_spill_label_t key = {.name = in->args};
      sm_i386_spill_label_t *found = bsearch(&key, labels, label_count,
          sizeof(*labels), sm_i386_spill_label_order);
      if (found) { in->target = found->line; lines[found->line].incoming = true; }
      else { in->all = true; unknown_branch = true; }
      in->terminal = !strcmp(in->op, "jmp") || !strcmp(in->op, "jmpl");
      in->conditional = !in->terminal;
    } else if (!strncmp(in->op, "loop", 4)) {
      in->all = true; unknown_branch = true;
    } else if (!strncmp(in->op, "ret", 3) || !strcmp(in->op, "ud2")) {
      in->terminal = true;
    }
    /* Only ordinary full-word references can be removed. Any other access
     * or address formation excludes all overlapping words conservatively. */
    for (const char *q = in->text; (q = strstr(q, "(%ebp)")); q += 6) {
      const char *number = sm_i386_home_number(in->text, q);
      char *parsed = NULL;
      long offset = strtol(number, &parsed, 10);
      if (number == q || parsed != q) {
        memset(banned, 1, slots);
        continue;
      }
      bool word = sm_i386_home_opcode(in->text) && offset % 4 == 0;
      for (size_t s = 0; !word && s < slots; ++s) {
        long home = -8L - (long)s * 4;
        if (home < offset + 32 && offset < home + 4) banned[s] = 1;
      }
      if (word && offset <= -8 && offset >= -4L - bytes && in->reads < 8)
        in->read[in->reads++] = (int)((-offset - 8) / 4);
    }
    char a[192], b[192];
    if (!strcmp(in->op, "movl") && sm_i386_spill_split(in, a, b)) {
      in->write = sm_i386_spill_slot(b, bytes);
      if (in->write >= 0) in->reads = 0;
    }
  }
  /* Fold an adjacent stack-word read/modify/write triplet into the x86 memory
   * form. No intervening instruction can observe an escaped stack address, and
   * the memory form has the same result and flag effects as the register form. */
  for (size_t i = 0; i + 2 < count && !getenv("NYC_DISABLE_I386_RMW_FOLD"); ++i) {
    sm_i386_spill_line_t *load = &lines[i], *update = &lines[i + 1],
                         *store = &lines[i + 2];
    if (load->removed || update->removed || store->removed ||
        strcmp(load->op, "movl") || strcmp(store->op, "movl")) continue;
    char source[192], reg[192], result[192], destination[192];
    if (!sm_i386_spill_split(load, source, reg) ||
        !sm_i386_spill_split(store, result, destination) ||
        strcmp(reg, result)) continue;
    int slot = sm_i386_spill_slot(source, bytes);
    if (slot < 0 || slot != store->write) continue;
    int target_reg = sm_i386_spill_register(reg);
    if (target_reg < 0 || strcmp(reg, sm_i386_gpr[target_reg]) ||
        strcmp(destination, source)) continue;
    const char *op = update->op;
    char operand[192];
    bool unary = !strcmp(op, "incl") || !strcmp(op, "decl") ||
                 !strcmp(op, "negl") || !strcmp(op, "notl");
    if (unary) {
      strcpy(operand, update->args ? update->args : "");
      if (strcmp(operand, reg)) continue;
    } else {
      if (strcmp(op, "addl") && strcmp(op, "subl") && strcmp(op, "andl") &&
          strcmp(op, "orl") && strcmp(op, "xorl")) continue;
      char update_source[192], update_dest[192];
      if (!sm_i386_spill_split(update, update_source, update_dest) ||
          strcmp(update_dest, reg) || sm_i386_spill_slot(update_source, bytes) >= 0)
        continue;
      int source_reg = sm_i386_spill_register(update_source);
      if (source_reg == target_reg ||
          (source_reg < 0 && update_source[0] != '$')) continue;
      strcpy(operand, update_source);
    }
    nyr_writer_t replacement = {0};
    bool formatted = unary
        ? nyr_printf(&replacement, "\t%s\t%s", op, source)
        : nyr_printf(&replacement, "\t%s\t%s, %s", op, operand, source);
    if (!formatted) { free(replacement.data); ok = false; goto done; }
    free(load->replacement);
    load->replacement = replacement.data;
    load->args = load->replacement + strlen("\t") + strlen(op) + 1;
    strcpy(load->op, op);
    update->removed = true;
    store->removed = true;
    load->reads = 1;
    load->read[0] = slot;
    load->write = slot;
  }
  /* Eliminate repeated private-home reloads within each straight-line block.
   * Aliased/volatile source memory is never cached. */
  int cached[8];
  for (int r = 0; r < 8; ++r) cached[r] = -1;
  for (size_t i = 0; i < count; ++i) {
    sm_i386_spill_line_t *in = &lines[i];
    if (!in->op[0]) {
      if (in->label && (in->incoming || unknown_branch))
        for (int r = 0; r < 8; ++r) cached[r] = -1;
      continue;
    }
    char a[192], b[192];
    bool split = sm_i386_spill_split(in, a, b);
    if (!strcmp(in->op, "movl") && split) {
      int ar = sm_i386_spill_register(a), br = sm_i386_spill_register(b);
      if (ar >= 6) ar = -1;
      if (br >= 6) br = -1;
      int as = sm_i386_spill_slot(a, bytes), bs = in->write;
      if (as >= 0 && banned[as]) as = -1;
      if (bs >= 0 && banned[bs]) bs = -1;
      if (br >= 0) {
        if ((as >= 0 && cached[br] == as) || ar == br) {
          in->removed = true; continue;
        }
        if (as >= 0) {
          for (int r = 0; r < 6; ++r) {
            if (cached[r] != as) continue;
            nyr_writer_t replacement = {0};
            if (!nyr_printf(&replacement, "\tmovl\t%s, %s",
                             sm_i386_gpr[r], sm_i386_gpr[br])) {
              free(replacement.data); ok = false; goto done;
            }
            in->replacement = replacement.data;
            in->args = in->replacement + strlen("\tmovl\t");
            in->reads = 0;
            break;
          }
        }
        cached[br] = ar >= 0 ? cached[ar] : as;
      }
      if (bs >= 0) {
        if (ar >= 0 && cached[ar] == bs) { in->removed = true; continue; }
        for (int r = 0; r < 8; ++r) if (cached[r] == bs) cached[r] = -1;
        if (ar >= 0) cached[ar] = bs;
      }
      continue;
    }
    if (!strncmp(in->op, "call", 4)) {
      cached[0] = cached[1] = cached[2] = -1;
    } else if (!strcmp(in->op, "cmpl") || !strcmp(in->op, "cmpw") ||
               !strcmp(in->op, "cmpb") || !strcmp(in->op, "testl") ||
               !strcmp(in->op, "testw") || !strcmp(in->op, "testb") ||
               !strcmp(in->op, "pushl")) {
      continue;
    } else if (split && sm_i386_spill_register(b) >= 0 &&
               (!strcmp(in->op, "addl") || !strcmp(in->op, "subl") ||
                !strcmp(in->op, "andl") || !strcmp(in->op, "orl") ||
                !strcmp(in->op, "xorl") || !strcmp(in->op, "imull") ||
                !strcmp(in->op, "leal") || !strncmp(in->op, "movz", 4) ||
                !strncmp(in->op, "movs", 4) || !strcmp(in->op, "movb") ||
                !strcmp(in->op, "movw") || !strcmp(in->op, "shll") ||
                !strcmp(in->op, "shrl") || !strcmp(in->op, "sarl") ||
                !strncmp(in->op, "cmov", 4))) {
      cached[sm_i386_spill_register(b)] = -1;
    } else {
      for (int r = 0; r < 8; ++r) cached[r] = -1;
    }
  }
  bool changed;
  do {
    changed = false;
    for (size_t at = count; at-- > 0;) {
      sm_i386_spill_line_t *in = &lines[at];
      memset(scratch, 0, words * sizeof(*scratch));
      if ((!in->terminal || in->removed) && at + 1 < count)
        memcpy(scratch, live + (at + 1) * words, words * sizeof(*scratch));
      if (!in->removed && in->target < count)
        for (size_t w = 0; w < words; ++w)
          scratch[w] |= live[in->target * words + w];
      if (!in->removed) {
        if (in->write >= 0)
          scratch[(size_t)in->write / 64] &= ~(UINT64_C(1) << (in->write % 64));
        for (int r = 0; r < in->reads; ++r)
          scratch[(size_t)in->read[r] / 64] |= UINT64_C(1) << (in->read[r] % 64);
        if (in->all) memset(scratch, 255, words * sizeof(*scratch));
      }
      if (memcmp(live + at * words, scratch, words * sizeof(*scratch))) {
        memcpy(live + at * words, scratch, words * sizeof(*scratch));
        changed = true;
      }
    }
  } while (changed);
  for (size_t i = 0; i < count; ++i) {
    sm_i386_spill_line_t *in = &lines[i];
    if (in->write >= 0 && !banned[in->write] && i + 1 < count &&
        !(live[(i + 1) * words + (size_t)in->write / 64] &
          (UINT64_C(1) << (in->write % 64)))) in->removed = true;
    if (in->terminal && in->target > i && in->target < count) {
      bool adjacent = true;
      for (size_t k = i + 1; k < in->target; ++k)
        adjacent &= lines[k].removed || !lines[k].op[0];
      if (adjacent) in->removed = true;
    }
  }
  /* Dead private spills can leave their source moves unused. Remove only
   * flag-preserving register definitions whose reads cannot be observable. */
  size_t call_cursor = 0;
  for (size_t i = 0; i < count; ++i) {
    sm_i386_spill_line_t *in = &lines[i];
    if (!in->op[0] || in->removed) continue;
    char a[192], b[192];
    bool split = sm_i386_spill_split(in, a, b);
    bool move = !strcmp(in->op, "movl") || !strcmp(in->op, "leal") ||
                !strcmp(in->op, "movzbl") || !strcmp(in->op, "movzwl") ||
                !strcmp(in->op, "movsbl") || !strcmp(in->op, "movswl");
    unsigned read = sm_i386_spill_register_mask(in->args);
    if (move && split) {
      int dst = sm_i386_spill_register(b);
      if (dst >= 0) {
        in->reg_write = (unsigned char)(1u << dst);
        read = sm_i386_spill_register_mask(a);
        int slot = sm_i386_spill_slot(a, bytes);
        in->reg_removable = dst < 6 &&
            (!strcmp(in->op, "leal") || a[0] == '$' ||
             sm_i386_spill_register(a) >= 0 ||
             (slot >= 0 && !banned[slot]));
      }
    } else if (!strncmp(in->op, "call", 4)) {
      read = 15u | 128u; in->reg_write = 7u;
      unsigned call_reads = 0;
      if (sm_i386_direct_call_reads(func, in->args, &call_cursor,
                                    &call_reads))
        read = call_reads | 128u;
    } else if (!strncmp(in->op, "ret", 3)) {
      read = 253u;
    } else if (in->op[0] == 'j') {
      read = in->all ? 255u :
             (!strcmp(in->op, "jecxz") || !strcmp(in->op, "jcxz") ? 2u : 0u);
    } else if (!strcmp(in->op, "cltd")) {
      read = 1u; in->reg_write = 4u;
    } else if (!strcmp(in->op, "popl")) {
      char pop_src[192], pop_dst[192];
      if (sm_i386_spill_split(in, pop_src, pop_dst)) {
        int dst = sm_i386_spill_register(pop_dst);
        if (dst >= 0) in->reg_write = (unsigned char)(1u << dst);
      }
      read = 128u;
    } else if (sm_i386_home_opcode(in->text) ||
               !strcmp(in->op, "movb") || !strcmp(in->op, "movw") ||
               !strcmp(in->op, "cmpb") || !strcmp(in->op, "cmpw") ||
               !strcmp(in->op, "testb") || !strcmp(in->op, "testw") ||
               !strcmp(in->op, "adcl") || !strcmp(in->op, "sbbl") ||
               !strcmp(in->op, "shll") || !strcmp(in->op, "shrl") ||
               !strcmp(in->op, "sarl") || !strcmp(in->op, "negl") ||
               !strcmp(in->op, "notl")) {
      if (!strcmp(in->op, "pushl") || !strcmp(in->op, "popl")) read |= 128u;
    } else if (in->op[0] != '.' || !strcmp(in->op, ".byte")) {
      read = 255u;
    }
    in->reg_read = (unsigned char)read;
  }
  do {
    changed = false;
    for (size_t at = count; at-- > 0;) {
      sm_i386_spill_line_t *in = &lines[at];
      unsigned out = (!in->terminal || in->removed) && at + 1 < count
                         ? lines[at + 1].reg_live : 0;
      if (!in->removed && in->target < count) out |= lines[in->target].reg_live;
      unsigned before = in->removed ? out :
          (out & ~(unsigned)in->reg_write) | in->reg_read;
      if (before != in->reg_live) {
        in->reg_live = (unsigned char)before; changed = true;
      }
    }
  } while (changed);
  /* Keep signed byte comparisons in byte width. The stack emitter commonly
   * extends a memory byte to 32 bits and immediately compares/tests it; CMPB
   * has identical signed conditions and avoids the extension and 32-bit op. */
  bool narrowed_compare = false;
  for (size_t i = 1; i < count; ++i) {
    sm_i386_spill_line_t *cmp = &lines[i];
    bool is_test = !strcmp(cmp->op, "testl");
    if (cmp->removed || (!is_test && strcmp(cmp->op, "cmpl")) || !cmp->args ||
        lines[i - 1].removed || !lines[i - 1].op[0] || lines[i - 1].label)
      continue;
    char lhs[192], rhs[192];
    if (!sm_i386_spill_split(cmp, lhs, rhs)) continue;
    const char *imm_text = NULL;
    const char *reg_text = NULL;
    long immediate = 0;
    if (is_test) {
      if (strcmp(lhs, rhs)) continue;
      reg_text = lhs;
    } else {
      if (lhs[0] == '$') { imm_text = lhs + 1; reg_text = rhs; }
      else if (rhs[0] == '$') { imm_text = rhs + 1; reg_text = lhs; }
      if (!imm_text || !reg_text) continue;
      char *imm_end = NULL;
      immediate = strtol(imm_text, &imm_end, 0);
      if (imm_end == imm_text || *imm_end || immediate < -128 || immediate > 255)
        continue;
    }
    int reg = sm_i386_spill_register(reg_text);
    if (reg < 0 || strcmp(reg_text, sm_i386_gpr[reg])) continue;
    unsigned reg_bit = 1u << reg;
    unsigned reg_out = (!cmp->terminal || cmp->removed) && i + 1 < count
                           ? lines[i + 1].reg_live : 0;
    if (cmp->target < count) reg_out |= lines[cmp->target].reg_live;
    if (reg_out & reg_bit) continue;

    size_t load_at = i - 1;
    sm_i386_spill_line_t *load = &lines[load_at];
    char source[192], destination[192];
    if (!sm_i386_spill_split(load, source, destination) ||
        strcmp(destination, sm_i386_gpr[reg]))
      continue;
    /* Fold a zero extension directly for zero tests, and fold a zero plus sign
     * extension pair when it represents a signed-byte comparison. */
    size_t first_load_at = load_at;
    bool zero_extended = !strcmp(load->op, "movzbl");
    if (is_test && zero_extended) {
      if (i + 1 >= count || lines[i + 1].removed || !lines[i + 1].conditional ||
          (strcmp(lines[i + 1].op, "je") && strcmp(lines[i + 1].op, "jz") &&
           strcmp(lines[i + 1].op, "jne") && strcmp(lines[i + 1].op, "jnz")))
        continue;
    } else if (!strcmp(load->op, "movsbl") && source[0] == '%') {
      if (load_at == 0) continue;
      sm_i386_spill_line_t *zero = &lines[load_at - 1];
      char low[192], wide[192];
      if (zero->removed || zero->label || strcmp(zero->op, "movzbl") ||
          !sm_i386_spill_split(zero, low, wide) || strcmp(wide, sm_i386_gpr[reg]))
        continue;
      static const char *const low_regs[] = {"%al", "%cl", "%dl", "%bl"};
      if (reg >= 4 || strcmp(low, low_regs[reg])) continue;
      first_load_at = load_at - 1;
      load = zero;
      strcpy(source, low);
      /* The byte source is operand one of the zero extension. */
      if (!sm_i386_spill_split(load, source, destination)) continue;
    } else if (!zero_extended && strcmp(load->op, "movsbl")) continue;
    if (zero_extended && !is_test) {
      if (immediate < 0 || immediate > 255 || i + 1 >= count ||
          lines[i + 1].removed || !lines[i + 1].conditional)
        continue;
      sm_i386_spill_line_t *branch = &lines[i + 1];
      bool signed_condition = !strcmp(branch->op, "jl") ||
                              !strcmp(branch->op, "jle") ||
                              !strcmp(branch->op, "jg") ||
                              !strcmp(branch->op, "jge");
      bool supported = signed_condition || !strcmp(branch->op, "je") ||
                       !strcmp(branch->op, "jz") || !strcmp(branch->op, "jne") ||
                       !strcmp(branch->op, "jnz") || !strcmp(branch->op, "jb") ||
                       !strcmp(branch->op, "jbe") || !strcmp(branch->op, "ja") ||
                       !strcmp(branch->op, "jae");
      if (!supported) continue;
      if (signed_condition) {
        if (!strcmp(branch->op, "jl")) strcpy(branch->op, "jb");
        else if (!strcmp(branch->op, "jle")) strcpy(branch->op, "jbe");
        else if (!strcmp(branch->op, "jg")) strcpy(branch->op, "ja");
        else strcpy(branch->op, "jae");
      }
    } else if (!is_test) {
      if (immediate < -128 || immediate > 127 || i + 1 >= count ||
          lines[i + 1].removed || !lines[i + 1].conditional)
        continue;
      const char *branch = lines[i + 1].op;
      if (strcmp(branch, "jl") && strcmp(branch, "jle") &&
          strcmp(branch, "jg") && strcmp(branch, "jge") &&
          strcmp(branch, "je") && strcmp(branch, "jz") &&
          strcmp(branch, "jne") && strcmp(branch, "jnz"))
        continue;
    }
    if (source[0] != '(' && !strstr(source, "(")) continue;
    if (strchr(source, ',') || strchr(source, '$')) continue;
    nyr_writer_t replacement = {0};
    if (!nyr_printf(&replacement, "\tcmpb\t$%ld, %s", immediate, source)) {
      free(replacement.data); ok = false; goto done;
    }
    free(cmp->replacement);
    cmp->replacement = replacement.data;
    cmp->args = cmp->replacement + strlen("\tcmpb\t");
    strcpy(cmp->op, "cmpb");
    cmp->reg_read = (unsigned char)sm_i386_spill_register_mask(source);
    load->removed = true;
    if (first_load_at != load_at) lines[first_load_at].removed = true;
    narrowed_compare = true;
  }
  /* Compare stack-home values in memory when the load's register dies at the
   * compare. This keeps operand order and lets x86 read the home directly. */
  for (size_t i = 1; i < count && !getenv("NYC_DISABLE_I386_HOME_CMP_FOLD"); ++i) {
    sm_i386_spill_line_t *cmp = &lines[i], *load = &lines[i - 1];
    if (cmp->removed || strcmp(cmp->op, "cmpl") || !cmp->args ||
        load->removed || strcmp(load->op, "movl") || load->label) continue;
    char lhs[192], rhs[192], source[192], destination[192];
    if (!sm_i386_spill_split(cmp, lhs, rhs) ||
        !sm_i386_spill_split(load, source, destination)) continue;
    int slot = sm_i386_spill_slot(source, bytes);
    int reg = sm_i386_spill_register(destination);
    if (slot < 0 || reg < 0 || strcmp(destination, sm_i386_gpr[reg])) continue;
    unsigned reg_bit = 1u << reg;
    unsigned reg_out = i + 1 < count ? lines[i + 1].reg_live : 0;
    if (cmp->target < count) reg_out |= lines[cmp->target].reg_live;
    if (reg_out & reg_bit) continue;
    bool lhs_value = !strcmp(lhs, destination), rhs_value = !strcmp(rhs, destination);
    if (lhs_value == rhs_value) continue;
    const char *new_lhs = lhs_value ? source : lhs;
    const char *new_rhs = rhs_value ? source : rhs;
    if (strchr(new_lhs, '(') && strchr(new_rhs, '(')) continue;
    nyr_writer_t replacement = {0};
    if (!nyr_printf(&replacement, "\tcmpl\t%s, %s", new_lhs, new_rhs)) {
      free(replacement.data); ok = false; goto done;
    }
    free(cmp->replacement);
    cmp->replacement = replacement.data;
    cmp->args = cmp->replacement + strlen("\tcmpl\t");
    cmp->reads = 1;
    cmp->read[0] = slot;
    load->removed = true;
  }
  if (narrowed_compare) {
    do {
      changed = false;
      for (size_t at = count; at-- > 0;) {
        sm_i386_spill_line_t *in = &lines[at];
        unsigned out = (!in->terminal || in->removed) && at + 1 < count
                           ? lines[at + 1].reg_live : 0;
        if (!in->removed && in->target < count) out |= lines[in->target].reg_live;
        unsigned before = in->removed ? out :
            (out & ~(unsigned)in->reg_write) | in->reg_read;
        if (before != in->reg_live) {
          in->reg_live = (unsigned char)before; changed = true;
        }
      }
    } while (changed);
  }
  bool folded_boolean_branch = false;
  for (size_t i = 0; i < count; ++i) {
    sm_i386_spill_line_t *set = &lines[i];
    if (unknown_branch || set->removed || set->label ||
        strncmp(set->op, "set", 3))
      continue;
    size_t extend_at = i + 1;
    while (extend_at < count && lines[extend_at].label &&
           !sm_i386_has_live_label_incoming(lines, count, extend_at))
      ++extend_at;
    if (extend_at >= count || lines[extend_at].label ||
        lines[extend_at].removed || strcmp(lines[extend_at].op, "movzbl"))
      continue;
    size_t test_at = extend_at + 1;
    while (test_at < count && lines[test_at].label &&
           !sm_i386_has_live_label_incoming(lines, count, test_at))
      ++test_at;
    if (test_at >= count || lines[test_at].label || lines[test_at].removed ||
        strcmp(lines[test_at].op, "testl"))
      continue;
    size_t branch_at = test_at + 1;
    while (branch_at < count && lines[branch_at].label &&
           !sm_i386_has_live_label_incoming(lines, count, branch_at))
      ++branch_at;
    if (branch_at >= count || lines[branch_at].label || lines[branch_at].removed)
      continue;
    sm_i386_spill_line_t *extend = &lines[extend_at];
    sm_i386_spill_line_t *test = &lines[test_at];
    sm_i386_spill_line_t *branch = &lines[branch_at];
    if (
        !branch->conditional ||
        (strcmp(branch->op, "jz") && strcmp(branch->op, "je") &&
         strcmp(branch->op, "jnz") && strcmp(branch->op, "jne")))
      continue;
    const char *direct = sm_i386_setcc_branch(set->op);
    if (!direct) continue;
    int byte_reg = sm_i386_spill_register(set->args);
    if (byte_reg < 0 || byte_reg >= 4 ||
        strcmp(set->args, sm_i386_gpr8[byte_reg]))
      continue;
    char source[192], destination[192], test_lhs[192], test_rhs[192];
    if (!sm_i386_spill_split(extend, source, destination) ||
        strcmp(source, sm_i386_gpr8[byte_reg]) ||
        strcmp(destination, sm_i386_gpr[byte_reg]) ||
        !sm_i386_spill_split(test, test_lhs, test_rhs) ||
        strcmp(test_lhs, sm_i386_gpr[byte_reg]) ||
        strcmp(test_rhs, test_lhs))
      continue;
    unsigned reg_out = (!branch->terminal || branch->removed) && branch_at + 1 < count
                           ? lines[branch_at + 1].reg_live : 0;
    if (branch->target < count) reg_out |= lines[branch->target].reg_live;
    if (reg_out & (1u << byte_reg)) continue;
    const char *replacement = direct;
    if (!strcmp(branch->op, "jz") || !strcmp(branch->op, "je")) {
      replacement = sm_i386_spill_inverse(direct);
      if (!replacement) continue;
    }
    strcpy(branch->op, replacement);
    set->removed = true;
    extend->removed = true;
    test->removed = true;
    folded_boolean_branch = true;
  }
  if (folded_boolean_branch) {
    do {
      changed = false;
      for (size_t at = count; at-- > 0;) {
        sm_i386_spill_line_t *in = &lines[at];
        unsigned out = (!in->terminal || in->removed) && at + 1 < count
                           ? lines[at + 1].reg_live : 0;
        if (!in->removed && in->target < count) out |= lines[in->target].reg_live;
        unsigned before = in->removed ? out :
            (out & ~(unsigned)in->reg_write) | in->reg_read;
        if (before != in->reg_live) {
          in->reg_live = (unsigned char)before; changed = true;
        }
      }
    } while (changed);
  }
  /* Propagate a register copy into its single consumer. A plain register
   * copy whose destination dies at the next instruction lets that
   * instruction read the original register, so the copy disappears without
   * ever mentioning a stack home (and without perturbing home coloring). */
  for (bool round_changed = true; round_changed &&
       !getenv("NYC_DISABLE_I386_COPY_PROPAGATE");) {
    round_changed = false;
    for (size_t i = 1; i < count; ++i) {
      sm_i386_spill_line_t *def = &lines[i - 1], *use = &lines[i];
      if (def->removed || def->label || use->removed || use->label ||
          strcmp(def->op, "movl") || !def->args || !use->args) continue;
      char src[192], dst[192];
      if (!sm_i386_spill_split(def, src, dst)) continue;
      int sreg = sm_i386_spill_register(src), dreg = sm_i386_spill_register(dst);
      if (sreg < 0 || dreg < 0 || sreg == dreg || sreg >= 6 || dreg >= 6 ||
          strcmp(src, sm_i386_gpr[sreg]) || strcmp(dst, sm_i386_gpr[dreg]))
        continue;
      unsigned dbit = 1u << dreg, sbit = 1u << sreg;
      if (!(use->reg_read & dbit) || (use->reg_write & (dbit | sbit)))
        continue;
      unsigned out = (i + 1 < count ? lines[i + 1].reg_live : 0) |
                     (use->target < count ? lines[use->target].reg_live : 0);
      if (out & dbit) continue;
      nyr_writer_t replacement = {0};
      bool formatted = nyr_printf(&replacement, "\t%s\t", use->op);
      const char *cursor = use->args;
      while (formatted && *cursor) {
        const char *hit = strstr(cursor, dst);
        if (!hit) { formatted = nyr_put(&replacement, cursor); break; }
        formatted =
            nyr_printf(&replacement, "%.*s%s", (int)(hit - cursor), cursor, src);
        if (!formatted) break;
        cursor = hit + strlen(dst);
      }
      if (!formatted) { free(replacement.data); ok = false; goto done; }
      free(use->replacement);
      use->replacement = replacement.data;
      use->args = use->replacement + 1 + strlen(use->op) + 1;
      use->reg_read = (unsigned char)((use->reg_read & ~dbit) | sbit);
      def->removed = true;
      round_changed = true;
    }
    if (round_changed) sm_i386_spill_reg_live(lines, count);
  }
  /* Forward a register through a dead copy: a definition of B followed by
   * a copy of B into C with B dead afterwards redefines C directly. Backward
   * sweeps fold whole copy chains; liveness is recomputed between rounds. */
  for (bool round_changed = true; round_changed &&
       !getenv("NYC_DISABLE_I386_MOVE_FORWARD");) {
    round_changed = false;
    for (size_t i = count; i-- > 1;) {
      sm_i386_spill_line_t *move_line = &lines[i],
                           *def_line = &lines[i - 1];
      if (move_line->removed || move_line->label || def_line->removed ||
          def_line->label || strcmp(move_line->op, "movl") ||
          !move_line->args || !def_line->op[0])
        continue;
      bool pop_def = !strcmp(def_line->op, "popl");
      bool cmov_def = !strncmp(def_line->op, "cmov", 4);
      if (!pop_def && !cmov_def && strcmp(def_line->op, "movl") &&
          strcmp(def_line->op, "leal") && strcmp(def_line->op, "movzbl") &&
          strcmp(def_line->op, "movzwl") && strcmp(def_line->op, "movsbl") &&
          strcmp(def_line->op, "movswl"))
        continue;
      char src[192], dst[192], copy_src[192], copy_dst[192];
      if (pop_def)
        strcpy(src, def_line->args ? def_line->args : "");
      else if (!sm_i386_spill_split(def_line, src, dst))
        continue;
      if (!pop_def && !strcmp(def_line->op, "movl") && src[0] != '$' &&
          sm_i386_spill_register(src) < 0)
        continue;
      int def_reg = sm_i386_spill_register(pop_def ? src : dst);
      if (def_reg < 0 || def_reg >= 6 ||
          strcmp(pop_def ? src : dst, sm_i386_gpr[def_reg]))
        continue;
      if (!sm_i386_spill_split(move_line, copy_src, copy_dst) ||
          strcmp(copy_src, sm_i386_gpr[def_reg])) continue;
      int copy_reg = sm_i386_spill_register(copy_dst);
      if (copy_reg < 0 || copy_reg >= 6 || copy_reg == def_reg ||
          strcmp(copy_dst, sm_i386_gpr[copy_reg]))
        continue;
      unsigned def_bit = 1u << def_reg;
      unsigned copy_out = (i + 1 < count ? lines[i + 1].reg_live : 0) |
                          (move_line->target < count
                               ? lines[move_line->target].reg_live : 0);
      if (copy_out & def_bit) continue;
      nyr_writer_t replacement = {0};
      bool formatted = pop_def
          ? nyr_printf(&replacement, "\tpopl\t%s", sm_i386_gpr[copy_reg])
          : nyr_printf(&replacement, "\t%s\t%s, %s", def_line->op, src,
                       sm_i386_gpr[copy_reg]);
      if (!formatted) { free(replacement.data); ok = false; goto done; }
      free(def_line->replacement);
      def_line->replacement = replacement.data;
      def_line->args =
          def_line->replacement + strlen("\t") + strlen(def_line->op) + 1;
      def_line->reg_write = (unsigned char)(1u << copy_reg);
      def_line->reg_read =
          pop_def ? 128u : (unsigned char)sm_i386_spill_register_mask(src);
      move_line->removed = true;
      round_changed = true;
    }
    if (round_changed) sm_i386_spill_reg_live(lines, count);
  }
  /* A push/sarl/popl bracket whose shift result was never stored only
   * clobbers and restores its register; drop it when no later instruction
   * can consume the shift's flags. */
  for (size_t i = 0; i + 2 < count &&
       !getenv("NYC_DISABLE_I386_SARL_BRACKET"); ++i) {
    sm_i386_spill_line_t *push = &lines[i], *sar = &lines[i + 1],
                        *pop = &lines[i + 2];
    if (push->removed || sar->removed || pop->removed ||
        strcmp(push->op, "pushl") || strcmp(sar->op, "sarl") ||
        strcmp(pop->op, "popl") || !push->args || !sar->args || !pop->args)
      continue;
    if (strcmp(push->args, pop->args)) continue;
    /* SARL names its register as the second operand ("$31, %eax"). */
    char shift_lhs[192], shift_rhs[192];
    if (!sm_i386_spill_split(sar, shift_lhs, shift_rhs)) continue;
    if (strcmp(shift_rhs, push->args)) continue;
    int reg = sm_i386_spill_register(push->args);
    if (reg < 0 || reg >= 6) continue;
    bool blocked = false;
    size_t hops = 0;
    for (size_t k = i + 3; k < count;) {
      const sm_i386_spill_line_t *scan = &lines[k];
      if (scan->removed || !scan->op[0]) { ++k; continue; }
      if (scan->label) {
        if (sm_i386_has_live_label_incoming(lines, count, k)) {
          blocked = true;
          break;
        }
        ++k;
        continue;
      }
      if (sm_i386_flag_consumer(scan->op)) { blocked = true; break; }
      if (sm_i386_flag_writer(scan->op)) break;
      if (!strcmp(scan->op, "jmp") && scan->target < count) {
        if (++hops > 16) { blocked = true; break; }
        k = scan->target;
        continue;
      }
      if (!strncmp(scan->op, "mov", 3) || !strcmp(scan->op, "leal") ||
          !strncmp(scan->op, "push", 4) || !strncmp(scan->op, "pop", 3) ||
          !strcmp(scan->op, "cltd") || !strcmp(scan->op, "nop")) {
        ++k;
        continue;
      }
      blocked = true;
      break;
    }
    if (blocked) continue;
    push->removed = true;
    sar->removed = true;
    pop->removed = true;
  }
  sm_i386_spill_reg_live(lines, count);
  for (size_t i = 0; i + 1 < count; ++i)
    if (lines[i].reg_removable && !(lines[i].reg_write & lines[i + 1].reg_live))
      lines[i].removed = true;
  for (size_t i = 0; i + 2 < count; ++i) {
    sm_i386_spill_line_t *in = &lines[i];
    const char *inverse = sm_i386_spill_inverse(in->op);
    if (!inverse || in->removed) continue;
    size_t next = i + 1;
    while (next < count && lines[next].removed) ++next;
    if (next >= count || strcmp(lines[next].op, "jmp") ||
        lines[next].target >= count || in->target <= next) continue;
    bool adjacent = true;
    for (size_t k = next + 1; k < in->target; ++k)
      adjacent &= lines[k].removed || lines[k].label;
    if (!adjacent) continue;
    nyr_writer_t replacement = {0};
    if (!nyr_printf(&replacement, "\t%s\t%s", inverse, lines[next].args)) {
      free(replacement.data); ok = false; goto done;
    }
    in->replacement = replacement.data;
    lines[next].removed = true;
  }
  nyr_writer_t cleaned = {0};
  size_t *map = calloc(slots, sizeof(*map));
  sm_i386_home_rank_t *rank = calloc(slots, sizeof(*rank));
  if (!map || !rank) {
    free(map); free(rank); ok = false; goto done;
  }
  size_t ranked = 0;
  size_t rank_limit = (size_t)value_bytes / 4;
  if (rank_limit > slots) rank_limit = slots;
  for (size_t s = 0; s < slots; ++s) {
    map[s] = s;
    if (s >= rank_limit || banned[s]) continue;
    rank[ranked++] = (sm_i386_home_rank_t){.slot = s};
  }
  for (size_t i = 0; i < count; ++i) {
    if (lines[i].removed) continue;
    const char *text = lines[i].replacement ? lines[i].replacement : lines[i].text;
    for (const char *scan = text; (scan = strstr(scan, "(%ebp)")); scan += 6) {
      const char *number = sm_i386_home_number(text, scan);
      char *end = NULL;
      long off = strtol(number, &end, 10);
      if (number != scan && end == scan && off <= -8 && off % 4 == 0 &&
          (uint64_t)(-off - 8) / 4 < slots)
        ++map[(size_t)(-off - 8) / 4];
    }
  }
  for (size_t r = 0; r < ranked; ++r) {
    size_t s = rank[r].slot;
    rank[r].weight = map[s] - s;
  }
  for (size_t s = 0; s < slots; ++s) map[s] = s;
  if (!colored && !getenv("NYC_DISABLE_I386_HOME_RANKING")) {
    qsort(rank, ranked, sizeof(*rank), sm_i386_home_rank_order);
    size_t r = 0;
    for (size_t s = 0; s < rank_limit; ++s)
      if (!banned[s]) map[rank[r++].slot] = s;
  }
  for (size_t i = 0; ok && i < count; ++i) {
    if (lines[i].removed) continue;
    /* Labels were temporarily nul-terminated without their colon. */
    ok = sm_i386_write_ranked_homes(&cleaned,
                    lines[i].replacement ? lines[i].replacement : lines[i].text,
                    map, slots) &&
         nyr_printf(&cleaned, "%s\n", lines[i].label ? ":" : "");
  }
  free(map); free(rank);
  if (ok && colored)
    ok = sm_i386_color_spill_homes(func, cleaned.data, colored, lines, count,
                                    slots, words, banned, live) >= 0;
  if (ok) { free(assembly->data); *assembly = cleaned; }
  else free(cleaned.data);
done:
  if (lines)
    for (size_t i = 0; i < count; ++i) free(lines[i].replacement);
  free(copy); free(lines); free(labels); free(banned); free(live); free(scratch);
  return ok;
}
