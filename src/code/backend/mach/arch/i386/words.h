/* Backward demand propagation over value/local edges. Cycles converge rather
 * than forcing every loop-carried C integer to occupy a full 64-bit home. */
static bool sm_i386_mark_high(unsigned char *bits, size_t count, int value) {
  if (value < 0 || (size_t)value >= count || bits[value])
    return false;
  bits[value] = 1;
  return true;
}

static void sm_i386_compute_high_words(ny_sm_ctx_t *c, unsigned char *values,
                                        unsigned char *locals, size_t local_count) {
  size_t count = (size_t)c->nyir->next_value;
  bool changed = true;
  while (changed) {
    changed = false;
    for (size_t n = c->nyir->len; n > 0; --n) {
      const nyr_inst_t *in = &c->nyir->data[n - 1];
      bool output = in->dst >= 0 && (size_t)in->dst < count && values[in->dst];
      bool unknown = false;
      switch (in->op) {
      case NYIR_NOP: case NYIR_LABEL: case NYIR_BR:
      case NYIR_CONST_I64: case NYIR_CONST_F32:
      case NYIR_ADDR_SYMBOL: case NYIR_ALLOCA:
        break;
      case NYIR_BR_IF:
      case NYIR_SELECT_I64: {
        if (sm_i386_truth_needs_high(c, in->a))
          changed |= sm_i386_mark_high(values, count, in->a);
        if (in->op == NYIR_SELECT_I64 && output) {
          changed |= sm_i386_mark_high(values, count, in->b);
          changed |= sm_i386_mark_high(values, count, in->c);
        }
        break;
      }
      case NYIR_CONST_F64:
        changed |= sm_i386_mark_high(values, count, in->dst);
        break;
      case NYIR_COPY:
        if (output) changed |= sm_i386_mark_high(values, count, in->a);
        break;
      case NYIR_PHI:
        if (output)
          for (size_t p = 0; p < in->phi_incoming_len; ++p)
            changed |= sm_i386_mark_high(values, count, in->phi_incoming[p].value);
        break;
      case NYIR_ADD_I64: case NYIR_SUB_I64: case NYIR_MUL_I64:
      case NYIR_XOR_I64:
        if (output && !(in->flags & NYIR_INST_F_NARROW32)) {
          changed |= sm_i386_mark_high(values, count, in->a);
          changed |= sm_i386_mark_high(values, count, in->b);
        }
        break;
      case NYIR_AND_I64: case NYIR_OR_I64: {
        int64_t immediate = 0;
        bool annihilates =
            (sm_i386_const_value(c, in->a, &immediate) ||
             sm_i386_const_value(c, in->b, &immediate)) &&
            (in->op == NYIR_AND_I64 ? ((uint64_t)immediate >> 32) == 0 :
                                      ((uint64_t)immediate >> 32) == UINT32_MAX);
        if (output && !annihilates && !(in->flags & NYIR_INST_F_NARROW32)) {
          changed |= sm_i386_mark_high(values, count, in->a);
          changed |= sm_i386_mark_high(values, count, in->b);
        }
        break;
      }
      case NYIR_SHL_I64: {
        int64_t shift = 0;
        if (output && !(sm_i386_const_value(c, in->b, &shift) &&
                        shift >= 32 && shift < 64))
          changed |= sm_i386_mark_high(values, count, in->a);
        break;
      }
      case NYIR_SAR_I64:
        changed |= sm_i386_mark_high(values, count, in->a);
        break;
      case NYIR_CMP_I64: {
        bool is_unsigned = false;
        if (!sm_i386_cmp32_mode(c, in, &is_unsigned)) {
          changed |= sm_i386_mark_high(values, count, in->a);
          changed |= sm_i386_mark_high(values, count, in->b);
        }
        break;
      }
      case NYIR_CMP_F64:
        changed |= sm_i386_mark_high(values, count, in->a);
        changed |= sm_i386_mark_high(values, count, in->b);
        break;
      case NYIR_CMP_F32:
        break;
      case NYIR_ADDR_LOCAL:
        changed |= sm_i386_mark_high(locals, local_count, (int)in->imm);
        break;
      case NYIR_LOAD_LOCAL:
        if (output)
          changed |= sm_i386_mark_high(locals, local_count, (int)in->imm);
        break;
      case NYIR_STORE_LOCAL:
        if (in->imm >= 0 && (uint64_t)in->imm < local_count && locals[in->imm])
          changed |= sm_i386_mark_high(values, count, in->a);
        break;
      case NYIR_LOAD_I64:
        break;
      case NYIR_STORE_I64:
        if (!(in->flags & (NYIR_INST_F_MEM_BYTE | NYIR_INST_F_MEM_U16 |
                           NYIR_INST_F_MEM_I32)))
          changed |= sm_i386_mark_high(values, count, in->c);
        break;
      case NYIR_RET:
        if (!(in->flags & NYIR_INST_F_I386_RET32)) {
          changed |= sm_i386_mark_high(values, count, in->a);
          changed |= sm_i386_mark_high(values, count, in->b);
        }
        break;
      case NYIR_CALL:
        /* Downstream wide consumers mark when EDX is needed; 32-bit C
         * returns leave EDX unspecified. */
        if (!(in->flags & NYIR_INST_F_I386_C_ABI)) {
          unknown = true;
          break;
        }
        for (size_t p = 0; p < (size_t)in->imm && p < NYIR_CALL_MAX_ARGS; ++p) {
          if (nyr_call_arg_class_get(in, p) != NYIR_SCALAR_F64 &&
              !(p < 64 && (in->i386_wide_args & (UINT64_C(1) << p))))
            continue;
          const int args[] = {in->a, in->b, in->c, in->d, in->e, in->f};
          int argument = p < 6 ? args[p] :
                         p - 6 < in->extra_args_len ? in->extra_args[p - 6] : -1;
          changed |= sm_i386_mark_high(values, count, argument);
        }
        break;
      case NYIR_INLINE_ASM:
        /* Only opaque asm or explicitly named outputs can touch a home
         * without ADDR_LOCAL. Recognized i386 register operands are 32-bit. */
        for (size_t p = 0; p < local_count; ++p)
          if (sm_i386_asm_touches_local(in, (int64_t)p))
            changed |= sm_i386_mark_high(locals, local_count, (int)p);
        changed |= sm_i386_mark_high(values, count, in->dst);
        if (in->imm == -21 || in->imm == -22 || in->imm == -23) {
          changed |= sm_i386_mark_high(values, count, in->a);
          if (in->imm != -23)
            changed |= sm_i386_mark_high(values, count, in->b);
        } else {
          unknown = !sm_i386_asm_low_operands(in);
        }
        break;
      default:
        unknown = true;
        break;
      }
      if (unknown) {
        changed |= sm_i386_mark_high(values, count, in->dst);
        const int args[] = {in->a, in->b, in->c, in->d, in->e, in->f};
        for (size_t p = 0; p < sizeof(args) / sizeof(args[0]); ++p)
          changed |= sm_i386_mark_high(values, count, args[p]);
        for (size_t p = 0; p < in->extra_args_len; ++p)
          changed |= sm_i386_mark_high(values, count, in->extra_args[p]);
      }
    }
  }
}
