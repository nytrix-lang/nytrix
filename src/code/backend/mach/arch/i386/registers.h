/* Local allocation for straight-line i386 integer runs. The surrounding
 * emitter owns frame layout and ABI lowering; every live-out has its normal
 * home at a run boundary. Nonvolatile registers are saved around each run. */
typedef struct {
  int reg;
  unsigned definitions;
  unsigned representation; /* 0 unknown, 1 low word, 2 signed32, 3 unsigned32, 4 wide */
  size_t last;
  bool outside;
  bool dirty;
} sm_i386_reg_value_t;

typedef struct {
  ny_sm_ctx_t *c;
  sm_i386_reg_value_t *values;
  int registers[6];
  unsigned pinned;
  unsigned used;
  size_t at;
  char stack_bias[224];
} sm_i386_regs_t;

static const char *const sm_i386_gpr[] = {
    "%eax", "%ecx", "%edx", "%ebx", "%esi", "%edi"};
static const char *const sm_i386_gpr8[] = {"%al", "%cl", "%dl", "%bl"};
static const char *const sm_i386_gpr16[] = {
    "%ax", "%cx", "%dx", "%bx", "%si", "%di"};

static bool sm_i386_reg_input(const sm_i386_regs_t *r, int value) {
  return value >= 0 && value < r->c->nyir->next_value;
}

static unsigned sm_i386_reg_representation(sm_i386_regs_t *r, int value) {
  if (value < 0 || value >= r->c->nyir->next_value)
    return 4;
  sm_i386_reg_value_t *v = &r->values[value];
  if (!v->representation) {
    nyr_range_t range = {0};
    if (!sm_i386_high_word_used_wide(r->c, value))
      v->representation = 1;
    else if (sm_i386_range_for_value(r->c, value, &range) &&
             range.min >= INT32_MIN && range.max <= INT32_MAX)
      v->representation = 2;
    else if (range.has_min && range.has_max && range.min >= 0 &&
             range.max <= UINT32_MAX)
      v->representation = 3;
    else
      v->representation = 4;
  }
  return v->representation;
}

static size_t sm_i386_reg_instruction(sm_i386_regs_t *r, size_t i) {
  const nyr_inst_t *in = &r->c->nyir->data[i];
  unsigned bits = 0;
  if (in->op == NYIR_LOAD_I64 && in->dst >= 0 &&
      in->dst < r->c->nyir->next_value &&
      ((in->flags & (NYIR_INST_F_MEM_BYTE | NYIR_INST_F_MEM_U16)) ==
           NYIR_INST_F_MEM_BYTE ||
       (in->flags & (NYIR_INST_F_MEM_BYTE | NYIR_INST_F_MEM_U16)) ==
           NYIR_INST_F_MEM_U16) &&
      i + 2 < r->c->nyir->len) {
    const nyr_inst_t *left = &r->c->nyir->data[i + 1];
    const nyr_inst_t *right = &r->c->nyir->data[i + 2];
    if (sm_i386_match_sign_extend(r->c, i + 1, &bits) &&
        left->a == in->dst &&
        bits == ((in->flags & NYIR_INST_F_MEM_BYTE) ? 8u : 16u) &&
        r->values[in->dst].definitions == 1 &&
        right->dst >= 0 && right->dst < r->c->nyir->next_value &&
        r->values[right->dst].definitions == 1 &&
        r->c->i386_uses->offsets[in->dst + 1] -
                r->c->i386_uses->offsets[in->dst] == 1 &&
        r->c->i386_uses->users[r->c->i386_uses->offsets[in->dst]] == i + 1 &&
        sm_i386_reg_input(r, in->a) &&
        sm_i386_reg_representation(r, right->dst) < 4)
      return 3;
  }
  if (sm_i386_match_sign_extend(r->c, i, &bits) &&
      r->values[in->dst].definitions == 1 &&
      r->values[r->c->nyir->data[i + 1].dst].definitions == 1 &&
      sm_i386_reg_input(r, in->a) &&
      sm_i386_reg_representation(r, r->c->nyir->data[i + 1].dst) < 4) {
    bool only_pair = true;
    for (size_t u = r->c->i386_uses->offsets[in->dst];
         u < r->c->i386_uses->offsets[in->dst + 1]; ++u)
      only_pair &= r->c->i386_uses->users[u] == i + 1;
    if (only_pair)
      return 2;
  }
  if (in->dst >= 0 &&
      (r->values[in->dst].definitions != 1 ||
       sm_i386_reg_representation(r, in->dst) == 4))
    return 0;
  switch (in->op) {
  case NYIR_CONST_I64:
  case NYIR_LOAD_LOCAL:
    return 1;
  case NYIR_ADDR_SYMBOL:
    return in->symbol && strncmp(in->symbol, "__nyc_local_label_", 18) != 0;
  case NYIR_ADDR_LOCAL:
    return in->imm >= 0 && in->imm < r->c->max_local_slot;
  case NYIR_ALLOCA:
    return r->c->i386_alloca_static;
  case NYIR_COPY:
    return sm_i386_reg_input(r, in->a);
  case NYIR_SELECT_I64:
    return sm_i386_reg_representation(r, in->a) < 4 &&
           sm_i386_reg_input(r, in->b) && sm_i386_reg_input(r, in->c);
  case NYIR_BR_IF:
    return sm_i386_reg_input(r, in->a) &&
           !sm_i386_truth_needs_high(r->c, in->a);
  case NYIR_SHL_I64:
  case NYIR_SAR_I64: {
    int64_t shift = 0;
    if (!sm_i386_reg_input(r, in->a) ||
        !sm_i386_const_value(r->c, in->b, &shift) || shift < 0 || shift >= 64)
      return 0;
    if (in->op == NYIR_SHL_I64) return 1;
    nyr_range_t range = {0};
    return sm_i386_range_for_value(r->c, in->a, &range) &&
           range.min >= INT32_MIN && range.max <= UINT32_MAX &&
           (range.max <= INT32_MAX || range.min >= 0);
  }
  case NYIR_ROR32_I64: {
    int64_t shift = 0;
    return sm_i386_reg_input(r, in->a) &&
           sm_i386_const_value(r->c, in->b, &shift) && shift >= 0 && shift < 32;
  }
  case NYIR_BSWAP32_I64:
    return sm_i386_reg_input(r, in->a);
  case NYIR_ADD_I64: case NYIR_SUB_I64: case NYIR_MUL_I64:
  case NYIR_AND_I64: case NYIR_OR_I64: case NYIR_XOR_I64:
  case NYIR_CMP_I64: {
    bool is_unsigned = false;
    if (in->op == NYIR_CMP_I64 && !sm_i386_cmp32_mode(r->c, in, &is_unsigned))
      return 0;
    return sm_i386_reg_input(r, in->a) && sm_i386_reg_input(r, in->b);
  }
  case NYIR_LOAD_I64:
    return (in->flags & (NYIR_INST_F_MEM_BYTE | NYIR_INST_F_MEM_U16 |
                         NYIR_INST_F_MEM_I32)) &&
           sm_i386_reg_input(r, in->a);
  case NYIR_STORE_LOCAL:
    return sm_i386_reg_input(r, in->a) &&
           (!sm_i386_local_high_word_used_wide(r->c, (int)in->imm) ||
            sm_i386_reg_representation(r, in->a) < 4);
  case NYIR_STORE_I64:
    return (in->flags & (NYIR_INST_F_MEM_BYTE | NYIR_INST_F_MEM_U16 |
                         NYIR_INST_F_MEM_I32)) &&
           sm_i386_reg_input(r, in->a) && sm_i386_reg_input(r, in->c);
  default:
    return 0;
  }
}

static bool sm_i386_reg_high(sm_i386_regs_t *r, int reg, int offset,
                              unsigned representation) {
  if (representation == 3)
    return sm_i386_store_u32(r->c, offset, 0);
  if (representation != 2)
    return false;
  const char *name = sm_i386_gpr[reg];
  return nyr_printf(r->c->w, "\tpushl\t%s\n\tsarl\t$31, %s\n", name, name) &&
         sm_st(r->c, offset, name) && nyr_printf(r->c->w, "\tpopl\t%s\n", name);
}

static bool sm_i386_reg_home(sm_i386_regs_t *r, int value) {
  sm_i386_reg_value_t *v = &r->values[value];
  if (!v->dirty)
    return true;
  if (!sm_st(r->c, sm_voff(r->c, value), sm_i386_gpr[v->reg]))
    return false;
  if (v->representation != 1 &&
      !sm_i386_reg_high(r, v->reg, sm_vhi_off(r->c, value), v->representation))
    return false;
  v->dirty = false;
  return true;
}

static int sm_i386_reg_allocate(sm_i386_regs_t *r, bool byte) {
  int limit = byte ? 4 : r->c->i386_register_limit;
  int chosen = -1;
  for (int reg = 0; reg < limit; ++reg) {
    if (r->pinned & (1u << reg))
      continue;
    int value = r->registers[reg];
    if (value < 0) {
      chosen = reg;
      break;
    }
    if (r->values[value].last < r->at) {
      chosen = reg;
      break;
    }
    if (chosen < 0 || r->values[value].last >
                        r->values[r->registers[chosen]].last)
      chosen = reg;
  }
  if (chosen < 0)
    return -1;
  int old = r->registers[chosen];
  if (old >= 0) {
    if ((r->values[old].outside || r->values[old].last >= r->at) &&
        !sm_i386_reg_home(r, old))
      return -1;
    r->values[old].reg = -1;
  }
  r->registers[chosen] = -1;
  r->used |= 1u << chosen;
  return chosen;
}

static int sm_i386_reg_load(sm_i386_regs_t *r, int value, bool byte) {
  sm_i386_reg_value_t *v = &r->values[value];
  int reg = v->reg;
  if (reg >= 0 && (!byte || reg < 4)) {
    r->pinned |= 1u << reg;
    return reg;
  }
  int target = sm_i386_reg_allocate(r, byte);
  if (target < 0)
    return -1;
  int64_t constant = 0;
  bool is_constant = reg < 0 && sm_i386_const_value(r->c, value, &constant);
  bool ok = reg >= 0
      ? nyr_printf(r->c->w, "\tmovl\t%s, %s\n", sm_i386_gpr[reg], sm_i386_gpr[target])
      /* Compare and branch paths materialize EFLAGS after loading operands. */
      : is_constant && (uint32_t)constant == 0
          ? nyr_printf(r->c->w, "\txorl\t%s, %s\n",
                       sm_i386_gpr[target], sm_i386_gpr[target])
          : is_constant && ((uint32_t)constant == 1 ||
                            (uint32_t)constant == UINT32_MAX)
              ? nyr_printf(r->c->w, "\txorl\t%s, %s\n\t%s\t%s\n",
                           sm_i386_gpr[target], sm_i386_gpr[target],
                           (uint32_t)constant == 1 ? "incl" : "decl",
                           sm_i386_gpr[target])
          : is_constant && (int32_t)(uint32_t)constant >= INT8_MIN &&
                (int32_t)(uint32_t)constant <= INT8_MAX
              ? nyr_printf(r->c->w, "\tpushl\t$%" PRId32 "\n\tpopl\t%s\n",
                           (int32_t)(uint32_t)constant, sm_i386_gpr[target])
          : is_constant && target < 4 &&
                (uint32_t)constant >= 128 && (uint32_t)constant <= 255
              ? nyr_printf(r->c->w, "\txorl\t%s, %s\n\tmovb\t$%u, %s\n",
                           sm_i386_gpr[target], sm_i386_gpr[target],
                           (unsigned)(uint32_t)constant,
                           sm_i386_gpr8[target])
          : is_constant
              ? nyr_printf(r->c->w, "\tmovl\t$0x%08" PRIx32 ", %s\n",
                           (uint32_t)constant, sm_i386_gpr[target])
              : sm_ld(r->c, sm_i386_gpr[target], sm_voff(r->c, value));
  if (!ok)
    return -1;
  if (reg >= 0)
    r->registers[reg] = -1;
  r->registers[target] = value;
  v->reg = target;
  r->pinned |= 1u << target;
  return target;
}

static int sm_i386_reg_result(sm_i386_regs_t *r, int dst, int source,
                               bool byte, bool copy_source) {
  int reg = source >= 0 ? r->values[source].reg : -1;
  if (reg >= 0 && (!byte || reg < 4) &&
      r->values[source].last <= r->at) {
    if (r->values[source].outside && !sm_i386_reg_home(r, source))
      return -1;
    r->values[source].reg = -1;
    r->registers[reg] = -1;
  } else {
    reg = sm_i386_reg_allocate(r, byte);
    if (reg < 0)
      return -1;
    if (copy_source && source >= 0 &&
        !nyr_printf(r->c->w, "\tmovl\t%s, %s\n",
                    sm_i386_gpr[r->values[source].reg], sm_i386_gpr[reg]))
      return -1;
  }
  r->values[dst].reg = reg;
  r->values[dst].dirty = true;
  r->registers[reg] = dst;
  r->pinned |= 1u << reg;
  return reg;
}

/* 0: leave this instruction to the general emitter; -1: allocation/output
 * failure; 1: emitted a complete run and advanced *next. */
static int sm_i386_emit_register_run(ny_sm_ctx_t *c, size_t start, size_t *next,
                                      int incoming_eax, bool unhomed) {
  size_t count = (size_t)c->nyir->next_value;
  sm_i386_regs_t r = {.c = c};
  r.values = calloc(count, sizeof(*r.values));
  if (!r.values)
    return -1;
  for (size_t v = 0; v < count; ++v)
    r.values[v].reg = -1;
  for (int reg = 0; reg < 6; ++reg)
    r.registers[reg] = -1;
  for (size_t i = 0; i < c->nyir->len; ++i)
    if (c->nyir->data[i].dst >= 0)
      ++r.values[c->nyir->data[i].dst].definitions;
  size_t end = start, step = 0;
  bool has_alloca = false;
  while (end < c->nyir->len && (step = sm_i386_reg_instruction(&r, end))) {
    has_alloca |= c->nyir->data[end].op == NYIR_ALLOCA;
    end += step;
  }
  /* Even single instructions can benefit when the result has no stack home
   * users beyond this run. */
  if (end - start < 1) {
    free(r.values);
    return 0;
  }
  for (size_t v = 0; v < count; ++v) {
    for (size_t u = c->i386_uses->offsets[v]; u < c->i386_uses->offsets[v + 1]; ++u) {
      size_t at = c->i386_uses->users[u];
      if (at < start || at >= end)
        r.values[v].outside = true;
      else if (at > r.values[v].last)
        r.values[v].last = at;
    }
  }
  if (incoming_eax >= 0) {
    r.registers[0] = incoming_eax;
    r.values[incoming_eax].reg = 0;
    r.values[incoming_eax].dirty = unhomed;
    sm_i386_reg_representation(&r, incoming_eax);
  }
  if (has_alloca)
    snprintf(r.stack_bias, sizeof(r.stack_bias), "%s_register_stack_%zu",
              c->epilogue, c->copy_serial++);
  nyr_writer_t body = {0};
  nyr_writer_t *output = c->w;
  c->w = &body;
  bool ok = true;
  const nyr_inst_t *comparison = NULL;
  int compare_a = -1, compare_b = -1;
  int64_t compare_immediate = 0;
  bool compare_has_immediate = false;
  for (size_t i = start; ok && i < end;) {
    const nyr_inst_t *in = &c->nyir->data[i];
    unsigned bits = 0;
    size_t length = sm_i386_reg_instruction(&r, i);
    r.at = i + length - 1;
    r.pinned = 0;
    int a = -1, b = -1, dst = in->dst, result = -1;
    if (in->op == NYIR_BR_IF) {
      /* Make homes valid for the taken edge, then keep the register mapping
       * for the linear fallthrough path in this run. */
      for (int reg = 0; ok && reg < 6; ++reg) {
        int value = r.registers[reg];
        if (value >= 0 && r.values[value].outside)
          ok = sm_i386_reg_home(&r, value);
      }
      const char *condition = "nz";
      if (ok && comparison) {
        bool is_unsigned = false;
        ok = sm_i386_cmp32_mode(c, comparison, &is_unsigned);
        condition = sm_i386_cmp_condition(comparison->cmp, is_unsigned);
        if (compare_has_immediate && (uint32_t)compare_immediate == 0)
          ok = ok && nyr_printf(c->w, "\ttestl\t%s, %s\n",
                                sm_i386_gpr[compare_a], sm_i386_gpr[compare_a]);
        else
          ok = ok && (compare_has_immediate
              ? nyr_printf(c->w, "\tcmpl\t$0x%08" PRIx32 ", %s\n",
                           (uint32_t)compare_immediate,
                           sm_i386_gpr[compare_a])
              : nyr_printf(c->w, "\tcmpl\t%s, %s\n",
                           sm_i386_gpr[compare_b], sm_i386_gpr[compare_a]));
        comparison = NULL;
      } else if (ok) {
        int branch_reg = sm_i386_reg_load(&r, in->a, false);
        ok = branch_reg >= 0 &&
             nyr_printf(c->w, "\ttestl\t%s, %s\n",
                        sm_i386_gpr[branch_reg], sm_i386_gpr[branch_reg]);
      }
      ok = ok && nyr_printf(c->w, "\tj%s\t%s_nyir_L%" PRId64 "\n",
                            condition, c->epilogue, in->imm);
      i += length;
      continue;
    } else if (length == 3) {
      const nyr_inst_t *load = in;
      const nyr_inst_t *extend = &c->nyir->data[i + 2];
      sm_i386_match_sign_extend(c, i + 1, &bits);
      a = sm_i386_reg_load(&r, load->a, false);
      result = a < 0 ? -1 : sm_i386_reg_result(&r, extend->dst,
                                               load->a, false, false);
      ok = result >= 0 &&
           nyr_printf(c->w, "\tmovs%sl\t(%s), %s\n",
                      bits == 8 ? "b" : "w", sm_i386_gpr[a],
                      sm_i386_gpr[result]);
    } else if (length == 2) {
      sm_i386_match_sign_extend(c, i, &bits);
      dst = c->nyir->data[i + 1].dst;
      a = sm_i386_reg_load(&r, in->a, bits == 8);
      result = a < 0 ? -1 : sm_i386_reg_result(&r, dst, in->a, false, true);
      ok = result >= 0;
      if (ok && bits < 32)
        ok = nyr_printf(c->w, "\tmovs%sl\t%s, %s\n", bits == 8 ? "b" : "w",
                         bits == 8 ? sm_i386_gpr8[a] : sm_i386_gpr16[a],
                         sm_i386_gpr[result]);
    } else if (in->op == NYIR_SHL_I64 || in->op == NYIR_SAR_I64 ||
               in->op == NYIR_ROR32_I64) {
      int64_t shift = 0;
      ok = sm_i386_const_value(c, in->b, &shift);
      a = ok ? sm_i386_reg_load(&r, in->a, false) : -1;
      result = a < 0 ? -1 : sm_i386_reg_result(&r, dst, in->a, false, true);
      ok = result >= 0;
      bool zero = in->op == NYIR_SHL_I64 && shift >= 32;
      bool logical = false;
      if (in->op == NYIR_SAR_I64) {
        nyr_range_t range = {0};
        ok = ok && sm_i386_range_for_value(c, in->a, &range);
        logical = range.min >= 0;
        if (shift >= 32) {
          zero = logical;
          shift = 31;
        }
      }
      if (ok && zero)
        ok = nyr_printf(c->w, "\txorl\t%s, %s\n", sm_i386_gpr[result], sm_i386_gpr[result]);
      else if (ok && shift)
        ok = nyr_printf(c->w, "\t%sl\t$%u, %s\n",
            in->op == NYIR_ROR32_I64 ? "ror" :
            in->op == NYIR_SHL_I64 ? "shl" : logical ? "shr" : "sar",
            (unsigned)shift, sm_i386_gpr[result]);
    } else if (in->op == NYIR_BSWAP32_I64) {
      a = sm_i386_reg_load(&r, in->a, false);
      result = a < 0 ? -1 : sm_i386_reg_result(&r, dst, in->a, false, true);
      ok = result >= 0 &&
           nyr_printf(c->w, "\tbswapl\t%s\n", sm_i386_gpr[result]);
    } else if (in->op == NYIR_ADDR_SYMBOL) {
      result = sm_i386_reg_result(&r, dst, -1, false, false);
      const char *symbol = in->symbol;
      if (!strncmp(symbol, "__nyc_extern_", sizeof("__nyc_extern_") - 1))
        symbol += sizeof("__nyc_extern_") - 1;
      ok = result >= 0;
      if (ok)
        ok = c->i386_position_independent
            ? nyr_printf(c->w, "\tcall\t1f\n1:\n\tpopl\t%s\n\taddl\t$%s-1b, %s\n",
                          sm_i386_gpr[result], symbol, sm_i386_gpr[result])
            : nyr_printf(c->w, "\tmovl\t$%s, %s\n", symbol, sm_i386_gpr[result]);
    } else if (in->op == NYIR_ADDR_LOCAL || in->op == NYIR_ALLOCA) {
      int offset = 0;
      if (in->op == NYIR_ADDR_LOCAL) {
        offset = sm_loff(c, (int)in->imm);
      } else {
        ok = sm_i386_alloca_offset(c, in, &offset);
      }
      result = ok ? sm_i386_reg_result(&r, dst, -1, false, false) : -1;
      ok = result >= 0 && (in->op == NYIR_ADDR_LOCAL
          ? nyr_printf(c->w, "\tleal\t%d(%s), %s\n", offset,
                       sm_i386_frame_reg(c), sm_i386_gpr[result])
          : nyr_printf(c->w, "\tleal\t%d+%s(%%esp), %s\n", offset,
                         r.stack_bias, sm_i386_gpr[result]));
    } else if (in->op == NYIR_CONST_I64) {
      /* Every low-word consumer rematerializes constants. Only an external
       * wide consumer needs the canonical high word written here. */
      if (r.values[dst].outside && r.values[dst].representation != 1)
        ok = sm_i386_store_u32(c, sm_vhi_off(c, dst),
                               (uint32_t)((uint64_t)in->imm >> 32));
    } else if (in->op == NYIR_LOAD_LOCAL) {
      result = sm_i386_reg_result(&r, dst, -1, false, false);
      ok = result >= 0;
      if (ok)
        ok = sm_ldl(c, sm_i386_gpr[result], (int)in->imm);
    } else if (in->op == NYIR_SELECT_I64) {
      a = sm_i386_reg_load(&r, in->a, false);
      b = a < 0 ? -1 : sm_i386_reg_load(&r, in->b, false);
      int other = b < 0 ? -1 : sm_i386_reg_load(&r, in->c, false);
      result = other < 0 ? -1 : sm_i386_reg_result(&r, dst, in->c, false, true);
      ok = result >= 0 && nyr_printf(c->w, "\ttestl\t%s, %s\n\tcmovnel\t%s, %s\n",
          sm_i386_gpr[a], sm_i386_gpr[a], sm_i386_gpr[b], sm_i386_gpr[result]);
    } else if (in->op == NYIR_STORE_I64) {
      a = sm_i386_reg_load(&r, in->a, false);
      b = a < 0 ? -1 : sm_i386_reg_load(&r, in->c,
                                       (in->flags & NYIR_INST_F_MEM_BYTE) != 0);
      ok = a >= 0 && b >= 0;
      if (ok)
        ok = nyr_printf(c->w, "\tmov%s\t%s, (%s)\n",
            in->flags & NYIR_INST_F_MEM_BYTE ? "b" :
            in->flags & NYIR_INST_F_MEM_U16 ? "w" : "l",
            in->flags & NYIR_INST_F_MEM_BYTE ? sm_i386_gpr8[b] :
            in->flags & NYIR_INST_F_MEM_U16 ? sm_i386_gpr16[b] : sm_i386_gpr[b],
            sm_i386_gpr[a]);
    } else if (in->op == NYIR_STORE_LOCAL) {
      a = sm_i386_reg_load(&r, in->a, false);
      ok = a >= 0 && sm_stl(c, (int)in->imm, sm_i386_gpr[a]);
      if (ok && sm_i386_local_high_word_used_wide(c, (int)in->imm))
        ok = sm_i386_reg_high(&r, a, sm_lhi_off(c, (int)in->imm),
                              r.values[in->a].representation);
    } else {
      int64_t immediate = 0;
      bool has_immediate = in->op != NYIR_COPY && in->op != NYIR_LOAD_I64 &&
                           sm_i386_const_value(c, in->b, &immediate);
      a = sm_i386_reg_load(&r, in->a, false);
      if (a >= 0 && !has_immediate && in->op != NYIR_COPY && in->op != NYIR_LOAD_I64)
        b = sm_i386_reg_load(&r, in->b, false);
      bool compare = in->op == NYIR_CMP_I64;
      if (compare && a >= 0 && (has_immediate || b >= 0) &&
          i + 1 < end && c->nyir->data[i + 1].op == NYIR_BR_IF &&
          c->nyir->data[i + 1].a == dst && c->i386_single_use[dst]) {
        comparison = in; compare_a = a; compare_b = b;
        compare_has_immediate = has_immediate; compare_immediate = immediate;
        i += length;
        continue;
      }
      result = a < 0 ? -1 : sm_i386_reg_result(&r, dst, in->a, compare,
                                               !compare && in->op != NYIR_LOAD_I64);
      ok = result >= 0;
      if (ok && in->op == NYIR_LOAD_I64)
        ok = nyr_printf(c->w, "\t%s\t(%s), %s\n",
            in->flags & NYIR_INST_F_MEM_BYTE ? "movzbl" :
            in->flags & NYIR_INST_F_MEM_U16 ? "movzwl" : "movl",
            sm_i386_gpr[a], sm_i386_gpr[result]);
      else if (ok && compare) {
        bool is_unsigned = false;
        ok = sm_i386_cmp32_mode(c, in, &is_unsigned);
        if (ok)
          ok = has_immediate
              ? nyr_printf(c->w, "\tcmpl\t$0x%08" PRIx32 ", %s\n",
                            (uint32_t)immediate, sm_i386_gpr[a])
              : b >= 0 && nyr_printf(c->w, "\tcmpl\t%s, %s\n",
                                      sm_i386_gpr[b], sm_i386_gpr[a]);
        ok = ok && nyr_printf(c->w, "\tset%s\t%s\n\tmovzbl\t%s, %s\n",
                 sm_i386_cmp_condition(in->cmp, is_unsigned), sm_i386_gpr8[result],
                 sm_i386_gpr8[result], sm_i386_gpr[result]);
      } else if (ok && in->op != NYIR_COPY) {
        const char *op = in->op == NYIR_ADD_I64 ? "add" :
                         in->op == NYIR_SUB_I64 ? "sub" :
                         in->op == NYIR_MUL_I64 ? "imul" :
                         in->op == NYIR_AND_I64 ? "and" :
                         in->op == NYIR_OR_I64 ? "or" : "xor";
        if (has_immediate) {
          bool identity = ((in->op == NYIR_ADD_I64 || in->op == NYIR_SUB_I64 ||
                            in->op == NYIR_OR_I64 || in->op == NYIR_XOR_I64) &&
                           (uint32_t)immediate == 0) ||
                          (in->op == NYIR_AND_I64 && (uint32_t)immediate == UINT32_MAX) ||
                          (in->op == NYIR_MUL_I64 && (uint32_t)immediate == 1);
          bool addsub = in->op == NYIR_ADD_I64 || in->op == NYIR_SUB_I64;
          bool one = (uint32_t)immediate == 1;
          bool minus_one = (uint32_t)immediate == UINT32_MAX;
          bool increment = (in->op == NYIR_ADD_I64 && one) ||
                           (in->op == NYIR_SUB_I64 && minus_one);
          bool decrement = (in->op == NYIR_SUB_I64 && one) ||
                           (in->op == NYIR_ADD_I64 && minus_one);
          ok = identity ||
               (addsub && increment
                    ? nyr_printf(c->w, "\tincl\t%s\n", sm_i386_gpr[result])
                    : addsub && decrement
                          ? nyr_printf(c->w, "\tdecl\t%s\n", sm_i386_gpr[result])
                          : nyr_printf(c->w, "\t%sl\t$0x%08" PRIx32 ", %s\n",
                                       op, (uint32_t)immediate,
                                       sm_i386_gpr[result]));
        } else {
          ok = b >= 0 && nyr_printf(c->w, "\t%sl\t%s, %s\n", op,
                                     sm_i386_gpr[b], sm_i386_gpr[result]);
        }
      }
    }
    i += length;
  }
  for (int reg = 0; ok && reg < 6; ++reg) {
    int v = r.registers[reg];
    if (v >= 0 && r.values[v].outside)
      ok = sm_i386_reg_home(&r, v);
  }
  if (comparison)
    ok = false;
  c->w = output;
  if (has_alloca) {
    int saved = 0;
    for (int reg = 3; reg < 6; ++reg)
      if (!c->i386_global_registers && (r.used & (1u << reg))) saved += 4;
    ok = ok && nyr_printf(output, "\t.set %s, %d\n", r.stack_bias, saved);
  }
  for (int reg = 3; ok && reg < 6; ++reg)
    if (!c->i386_global_registers && (r.used & (1u << reg)))
      ok = nyr_printf(output, "\tpushl\t%s\n", sm_i386_gpr[reg]);
  ok = ok && nyr_put(output, body.data ? body.data : "");
  for (int reg = 5; ok && reg >= 3; --reg)
    if (!c->i386_global_registers && (r.used & (1u << reg)))
      ok = nyr_printf(output, "\tpopl\t%s\n", sm_i386_gpr[reg]);
  free(body.data);
  free(r.values);
  *next = end;
  return ok ? 1 : -1;
}
