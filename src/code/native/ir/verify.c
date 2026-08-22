/*
 * NYIR verifier: structural and semantic sanity checks on the IR
 * after each transform pass to catch malformed ops or type mismatches.
 */
#include "code/native/ir/internal.h"
#include "code/native/ir/opt/util.h"
#include "base/compat.h"
#include "base/common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool nir_value_valid(const nyir_func_t *f, int v) {
  return v >= 0 && v < f->next_value;
}

static bool nir_verify_inst_err(const nyir_func_t *f, char *err, size_t err_len,
                                const nyir_inst_t *in, size_t index,
                                const char *reason) {
  char detail[384];
  const char *why = reason ? reason : "invalid instruction";
  nyir_cfg_t cfg = {0};
  if (f && index < f->len && nyir_cfg_build_topology(f, &cfg) &&
      cfg.inst_block && index < f->len) {
    size_t block = cfg.inst_block[index];
    if (block < cfg.block_count && cfg.block_label && cfg.block_label[block] >= 0)
      snprintf(detail, sizeof(detail), "%s [block=%zu label=L%lld]", why, block,
               (long long)cfg.block_label[block]);
    else
      snprintf(detail, sizeof(detail), "%s [block=%zu]", why, block);
    nyir_cfg_free(&cfg);
    return nyir_inst_err(err, err_len, in, index, detail);
  }
  nyir_cfg_free(&cfg);
  return nyir_inst_err(err, err_len, in, index, why);
}

static bool nir_value_defined(const nyir_func_t *f, const bool *defined,
                              int v) {
  return nir_value_valid(f, v) && defined && defined[v];
}

static bool nir_label_exists(const nyir_func_t *f, int64_t label) {
  if (!f)
    return false;
  for (size_t i = 0; i < f->len; ++i) {
    const nyir_inst_t *in = &f->data[i];
    if (in->op == NYIR_LABEL && in->imm == label)
      return true;
  }
  return false;
}

bool nyir_label_referenced(const nyir_func_t *f, int64_t label) {
  if (!f)
    return false;
  for (size_t i = 0; i < f->len; ++i) {
    const nyir_inst_t *in = &f->data[i];
    if ((in->op == NYIR_BR || in->op == NYIR_BR_IF) && in->imm == label)
      return true;
  }
  return false;
}

static unsigned nir_known_effect_mask(void) {
  return NYIR_EFFECT_READ_LOCAL | NYIR_EFFECT_WRITE_LOCAL |
         NYIR_EFFECT_CALL | NYIR_EFFECT_CONTROL |
         NYIR_EFFECT_READ_MEMORY | NYIR_EFFECT_WRITE_MEMORY |
         NYIR_EFFECT_MAY_TRAP | NYIR_EFFECT_VOLATILE |
         NYIR_EFFECT_ALLOCATION | NYIR_EFFECT_UNKNOWN_SIDE_EFFECT |
         NYIR_EFFECT_IO | NYIR_EFFECT_THREAD | NYIR_EFFECT_FFI |
         NYIR_EFFECT_FENV;
}

/*
 * Instruction flags are part of NYIR's typed ABI metadata.  Keeping their
 * legality here prevents a malformed bundle from silently changing a call or
 * return register class in Machine IR lowering.
 */
static bool nir_flags_valid(const nyir_inst_t *in) {
  if (!in)
    return false;
  unsigned allowed = 0;
  switch (in->op) {
  case NYIR_CALL:
    allowed = NYIR_INST_F_EXTERN | NYIR_INST_F_RET_F64 |
              NYIR_INST_F_RET_F32 | NYIR_INST_F_SRET |
              NYIR_INST_F_EFFECTS_KNOWN;
    break;
  case NYIR_RET:
    allowed = NYIR_INST_F_RET_F64 | NYIR_INST_F_RET_F32;
    break;
  case NYIR_LOAD_I64:
  case NYIR_STORE_I64:
    allowed = NYIR_INST_F_MEM_F64 | NYIR_INST_F_MEM_BYTE;
    break;
  case NYIR_ADD_I64:
  case NYIR_SUB_I64:
  case NYIR_MUL_I64:
  case NYIR_AND_I64:
  case NYIR_OR_I64:
  case NYIR_XOR_I64:
    /*
     * F_NARROW32 is set by the range-narrowing pass (opt/narrow.c) so the
     * codegen backend may emit 32-bit register forms.  The internal x64
     * encoder ignores it (emitting 64-bit ops, still correct); the scalar
     * machine bridge honors it.  Either way the flag is legal here.
     */
    allowed = NYIR_INST_F_NARROW32;
    break;
  default:
    return in->flags == 0;
  }
  return (in->flags & ~allowed) == 0 &&
         !((in->flags & NYIR_INST_F_MEM_F64) &&
           (in->flags & NYIR_INST_F_MEM_BYTE)) &&
         !((in->flags & NYIR_INST_F_RET_F64) &&
           (in->flags & NYIR_INST_F_RET_F32));
}

/*
 * PHI operands live on CFG edges, not at their linear instruction position.
 * Keep this check separate from the scalar operand verifier so malformed
 * incoming sets cannot reach SSA destruction or a native backend.
 */
static bool nir_verify_phi_edges(const nyir_func_t *f, char *err,
                                 size_t err_len) {
  nyir_cfg_t cfg = {0};
  if (!nyir_cfg_build(f, &cfg))
    return nyir_err(err, err_len, "native NYIR verify: cannot build CFG");
  for (size_t i = 0; i < f->len; ++i) {
    const nyir_inst_t *phi = &f->data[i];
    if (phi->op != NYIR_PHI)
      continue;
    size_t block = cfg.inst_block[i];
    size_t predecessors = cfg.pred_offsets[block + 1] - cfg.pred_offsets[block];
    if (phi->phi_incoming_len != predecessors) {
      nyir_cfg_free(&cfg);
      return nir_verify_inst_err(f, err, err_len, phi, i,
                             "phi incoming edges do not match CFG predecessors");
    }
    for (size_t k = 0; k < phi->phi_incoming_len; ++k) {
      bool matched = false;
      for (size_t edge = cfg.pred_offsets[block];
           edge < cfg.pred_offsets[block + 1]; ++edge) {
        size_t p = cfg.pred_blocks[edge];
        if (cfg.block_label[p] == phi->phi_incoming[k].predecessor_label) {
          matched = true;
          break;
        }
      }
      if (!matched) {
        nyir_cfg_free(&cfg);
        return nir_verify_inst_err(f, err, err_len, phi, i,
                               "phi incoming label is not a CFG predecessor");
      }
    }
  }
  nyir_cfg_free(&cfg);
  return true;
}

static bool nir_verify_cfg_structure(const nyir_func_t *f, char *err,
                                     size_t err_len) {
  nyir_cfg_t cfg = {0};
  if (!nyir_cfg_build(f, &cfg))
    return nyir_err(err, err_len, "native NYIR verify: cannot build CFG");
  for (size_t block = 0; block < cfg.block_count; ++block) {
    /*
     * Unreachable blocks are a normal transient state after SCCP or branch
     * folding.  They are still checked structurally below, but CFG cleanup
     * owns their deletion; rejecting them here would make it impossible to
     * verify after every individual transformation.
     */
    size_t end = cfg.block_end[block];
    while (end > cfg.block_start[block] && f->data[end - 1].op == NYIR_NOP)
      --end;
    if (end == cfg.block_start[block])
      continue;
    nyir_op_t op = f->data[end - 1].op;
    if ((op == NYIR_BR || op == NYIR_BR_IF || op == NYIR_RET) &&
        end != cfg.block_end[block]) {
      nyir_cfg_free(&cfg);
      return nir_verify_inst_err(f, err, err_len, &f->data[end - 1], end - 1,
                             "terminator is followed by a non-NOP instruction");
    }
  }
  nyir_cfg_free(&cfg);
  return true;
}

static bool cfg_dominates_bit_debug(const nyir_cfg_t *cfg, size_t dominator,
                                    size_t dominated) {
  if (!cfg || dominator >= cfg->block_count || dominated >= cfg->block_count)
    return false;
  return nyir_cfg_dominates(cfg, dominator, dominated);
}

static bool nir_value_dominates_use(const nyir_func_t *f, const nyir_cfg_t *cfg,
                                     const int *definitions, int value,
                                     size_t use, char *err, size_t err_len,
                                     const nyir_inst_t *in) {
  if (value < 0 || !definitions || definitions[value] < 0)
    return nir_verify_inst_err(f, err, err_len, in, use, "value has no definition");
  size_t def = (size_t)definitions[value];
  size_t def_block = cfg->inst_block[def];
  size_t use_block = cfg->inst_block[use];
  /*
   * SCCP may leave structurally valid, unreachable edge blocks until CFG
   * cleanup. They cannot execute, so executable dominance is not defined for
   * their uses. Keep definition and structural checks intact.
   */
  if (!cfg->reachable[use_block])
    return true;
  if ((def_block == use_block && def < use) ||
      (def_block != use_block && nyir_cfg_dominates(cfg, def_block, use_block)))
    return true;
  if (getenv("NY_DEBUG_DOM")) {
    fprintf(stderr, "[dom-debug] def_block=%zu (label=%lld reachable=%d) "
                    "use_block=%zu (label=%lld reachable=%d)\n",
            def_block, (long long)cfg->block_label[def_block],
            cfg->reachable[def_block], use_block,
            (long long)cfg->block_label[use_block], cfg->reachable[use_block]);
    for (size_t b = 0; b < cfg->block_count; ++b) {
      fprintf(stderr, "  block %zu label=%lld reachable=%d idom=%d succ=[",
              b, (long long)cfg->block_label[b], cfg->reachable[b],
              cfg->idom ? cfg->idom[b] : -2);
      for (size_t si = cfg->succ_offsets[b]; si < cfg->succ_offsets[b + 1]; ++si)
        fprintf(stderr, "%zu ", cfg->succ_blocks[si]);
      fprintf(stderr, "] pred=[");
      for (size_t pi = cfg->pred_offsets[b]; pi < cfg->pred_offsets[b + 1]; ++pi)
        fprintf(stderr, "%zu ", cfg->pred_blocks[pi]);
      fprintf(stderr, "] dominates_use_block=%d\n",
              cfg_dominates_bit_debug(cfg, use_block, b));
    }
  }
  return nir_verify_inst_err(f, err, err_len, in, use,
                         "value definition does not dominate its use");
}

static bool nir_verify_dominance(const nyir_func_t *f, char *err,
                                 size_t err_len) {
  nyir_cfg_t cfg = {0};
  if (!nyir_cfg_build(f, &cfg))
    return nyir_err(err, err_len, "native NYIR verify: cannot build CFG");
  /*
   * value -> defining instruction index, used by the dominance checks
   * below.  NULL when next_value <= 0; the CHECK_DOMINATES paths already
   * tolerate a NULL map (they test definitions[value] < 0 first).  The
   * fill loop previously ran unconditionally outside the next_value > 0
   * guard; the helper now owns both the guard and the dst bounds check.
   */
  int *definitions = nyir_build_defs(f);
  if (f->next_value > 0 && !definitions) {
    nyir_cfg_free(&cfg);
    return nyir_err(err, err_len, "native NYIR verify: out of memory");
  }

#define CHECK_DOMINATES(value)                                                \
  do {                                                                        \
    if (!nir_value_dominates_use(f, &cfg, definitions, (value), i, err, err_len, \
                                 in))                                         \
      goto fail;                                                              \
  } while (0)

  for (size_t i = 0; i < f->len; ++i) {
    const nyir_inst_t *in = &f->data[i];
    switch (in->op) {
    case NYIR_PHI: {
      size_t phi_block = cfg.inst_block[i];
      for (size_t k = 0; k < in->phi_incoming_len; ++k) {
        int pred = -1;
        for (size_t b = 0; b < cfg.block_count; ++b)
          if (cfg.block_label[b] == in->phi_incoming[k].predecessor_label) {
            pred = (int)b;
            break;
          }
        int value = in->phi_incoming[k].value;
        if (pred >= 0 && !cfg.reachable[pred])
          continue;
        if (pred < 0 || value < 0 || !definitions || definitions[value] < 0 ||
            !nyir_cfg_dominates(&cfg,
                                  cfg.inst_block[(size_t)definitions[value]],
                                  (size_t)pred)) {
          nir_verify_inst_err(f, err, err_len, in, i,
                          "phi value does not dominate its predecessor edge");
          goto fail;
        }
        (void)phi_block;
      }
      break;
    }
    case NYIR_COPY:
    case NYIR_I64_TO_F64:
    case NYIR_I64_TO_F32:
    case NYIR_F64_TO_F32:
    case NYIR_F32_TO_F64:
    case NYIR_SQRT_F64:
    case NYIR_SIN_F64:
    case NYIR_COS_F64:
    case NYIR_LOAD_I64:
    case NYIR_VEC4_LOAD_I64:
    case NYIR_VEC8_LOAD_I64:
    case NYIR_VEC4_LOAD_F64:
    case NYIR_VEC8_LOAD_F32:
    case NYIR_VEC4_SET1_F64:
    case NYIR_VEC8_SET1_F32:
    case NYIR_VEC4_SET1_I64:
    case NYIR_VEC4_SHUFFLE_F64:
    case NYIR_VEC8_SHUFFLE_F32:
    case NYIR_STORE_LOCAL:
    case NYIR_RET:
    case NYIR_BR_IF:
      if (in->a >= 0) CHECK_DOMINATES(in->a);
      break;
    case NYIR_STORE_I64:
      CHECK_DOMINATES(in->a);
      CHECK_DOMINATES(in->c);
      break;
    case NYIR_VEC4_STORE_I64:
    case NYIR_VEC8_STORE_I64:
    case NYIR_VEC4_STORE_F64:
    case NYIR_VEC8_STORE_F32:
      CHECK_DOMINATES(in->a);
      CHECK_DOMINATES(in->b >= 0 ? in->b : in->c);
      break;
    case NYIR_VEC4_FMA_F64:
    case NYIR_VEC8_FMA_F32:
      CHECK_DOMINATES(in->a);
      CHECK_DOMINATES(in->b);
      CHECK_DOMINATES(in->c);
      break;
    case NYIR_CALL: {
      int args[6] = {in->a, in->b, in->c, in->d, in->e, in->f};
      size_t direct = in->imm < 6 ? (size_t)in->imm : 6;
      for (size_t arg = 0; arg < direct; ++arg)
        CHECK_DOMINATES(args[arg]);
      for (size_t arg = 0; arg < in->extra_args_len; ++arg)
        CHECK_DOMINATES(in->extra_args[arg]);
      break;
    }
    case NYIR_NOP:
    case NYIR_CONST_I64:
    case NYIR_LABEL:
    case NYIR_LOAD_LOCAL:
    case NYIR_BR:
    case NYIR_CONST_F64:
    case NYIR_CONST_F32:
    case NYIR_ADDR_LOCAL:
    case NYIR_ADDR_SYMBOL:
    case NYIR_ALLOCA:
    case NYIR_CAPTURE_RET:
      break;
    case NYIR_BOUNDS_CHECK:
      if (in->a >= 0) CHECK_DOMINATES(in->a);
      if (in->b >= 0) CHECK_DOMINATES(in->b);
      if (in->c >= 0) CHECK_DOMINATES(in->c);
      break;
    default:
      CHECK_DOMINATES(in->a);
      CHECK_DOMINATES(in->b);
      break;
    }
  }
#undef CHECK_DOMINATES
  free(definitions);
  nyir_cfg_free(&cfg);
  return true;
fail:
#undef CHECK_DOMINATES
  free(definitions);
  nyir_cfg_free(&cfg);
  return false;
}

bool nyir_verify(const nyir_func_t *f, char *err, size_t err_len) {
  if (!f)
    return nyir_err(err, err_len, "native NYIR verify: missing function");
  if (f->next_value < 0)
    return nyir_err(err, err_len, "native NYIR verify: invalid value count");
  if (f->param_count && !f->param_types)
    return nyir_err(err, err_len, "native NYIR verify: missing parameter types");
  for (size_t i = 0; i < f->param_count; ++i)
    if (f->param_types[i] > NYIR_PARAM_F64)
      return nyir_err(err, err_len, "native NYIR verify: invalid parameter type");
  bool *defined = NULL;
  if (f->next_value > 0) {
    defined = (bool *)calloc((size_t)f->next_value, sizeof(bool));
    if (!defined)
      return nyir_err(err, err_len, "native NYIR verify: out of memory");
  }
  for (size_t i = 0; i < f->len; ++i) {
    const nyir_inst_t *in = &f->data[i];
    if (in->op != NYIR_CALL && in->arg_sizes) {
      free(defined);
      return nir_verify_inst_err(f, err, err_len, in, i,
                             "non-call instruction has aggregate argument metadata");
    }
    if (in->op < 0 || in->op >= NYIR_OP_COUNT) {
      free(defined);
      return nir_verify_inst_err(f, err, err_len, in, i, "unknown opcode");
    }
    if (!nir_flags_valid(in)) {
      free(defined);
      return nir_verify_inst_err(f, err, err_len, in, i,
                             "invalid instruction flags");
    }
    if (in->op == NYIR_PHI) {
      /*
       * SSA joins are a block prefix.  Keeping that invariant explicit makes
       * edge use/dominance checks unambiguous and is required by PHI lowering.
       * NOPs are tolerated because in-place DCE may leave them temporarily.
       */
      size_t prev = i;
      while (prev > 0 && f->data[prev - 1].op == NYIR_NOP)
        --prev;
      if (prev > 0 && f->data[prev - 1].op != NYIR_LABEL &&
          f->data[prev - 1].op != NYIR_PHI) {
        free(defined);
        return nir_verify_inst_err(f, err, err_len, in, i,
                               "phi is not at the start of its block");
      }
    }
    if ((in->op == NYIR_CMP_I64 || in->op == NYIR_CMP_F64 ||
         in->op == NYIR_CMP_F32) &&
        in->cmp > NYIR_CMP_GE) {
      free(defined);
      return nir_verify_inst_err(f, err, err_len, in, i, "unknown comparison predicate");
    }
    unsigned required_effects = nyir_inst_effects(in);
    unsigned known_effects = nir_known_effect_mask();
    if ((in->effects & ~known_effects) != 0) {
      free(defined);
      return nir_verify_inst_err(f, err, err_len, in, i, "invalid effect mask");
    }
    bool trusted_call_ok =
        in->op == NYIR_CALL && (in->flags & NYIR_INST_F_EFFECTS_KNOWN) &&
        (in->effects & NYIR_EFFECT_CALL) != 0;
    if (in->effects != required_effects && !trusted_call_ok) {
      char detail[192];
      snprintf(detail, sizeof(detail),
               "effect mask does not match opcode effects"
               " (actual=0x%x required=0x%x symbol=%s flags=0x%x)",
               in->effects, required_effects,
               in->symbol ? in->symbol : "<none>", in->flags);
      free(defined);
      return nir_verify_inst_err(f, err, err_len, in, i, detail);
    }
    if ((in->range.has_min && !in->range.has_max) ||
        (!in->range.has_min && in->range.has_max)) {
      free(defined);
      return nir_verify_inst_err(f, err, err_len, in, i, "incomplete range fact");
    }
    if (in->range.has_min && in->range.has_max && in->range.min > in->range.max) {
      free(defined);
      return nir_verify_inst_err(f, err, err_len, in, i, "invalid range fact");
    }
    if (in->dst < 0 && (in->range.has_min || in->range.has_max)) {
      free(defined);
      return nir_verify_inst_err(f, err, err_len, in, i,
                           "range fact attached to non-value instruction");
    }
    if (in->op == NYIR_CONST_I64 && in->range.has_min && in->range.has_max &&
        (in->imm < in->range.min || in->imm > in->range.max)) {
      free(defined);
      return nir_verify_inst_err(f, err, err_len, in, i,
                           "constant is excluded by its range fact");
    }
    if ((!in->debug.line && in->debug.column) ||
        (!in->debug.line && in->debug.file && in->debug.file[0])) {
      free(defined);
      return nir_verify_inst_err(f, err, err_len, in, i, "invalid debug location");
    }
    if (in->dst >= f->next_value) {
      free(defined);
      return nir_verify_inst_err(f, err, err_len, in, i, "invalid destination value");
    }
    if (in->dst >= 0 && defined && defined[in->dst]) {
      free(defined);
      return nir_verify_inst_err(f, err, err_len, in, i,
                          "destination value is already defined");
    }
    switch (in->op) {
    case NYIR_NOP:
    case NYIR_BOUNDS_CHECK:
      break;
    case NYIR_LABEL:
      for (size_t j = 0; j < i; ++j) {
        if (f->data[j].op == NYIR_LABEL && f->data[j].imm == in->imm) {
          free(defined);
          return nir_verify_inst_err(f, err, err_len, in, i, "duplicate label");
        }
      }
      break;
    case NYIR_CONST_I64:
    case NYIR_CONST_F64:
    case NYIR_CONST_F32:
      if (in->dst < 0) {
        free(defined);
        return nir_verify_inst_err(f, err, err_len, in, i, "constant has no destination");
      }
      break;
  case NYIR_SQRT_F64:
  case NYIR_SIN_F64:
  case NYIR_COS_F64:
    if (in->dst < 0 || !nir_value_defined(f, defined, in->a)) {
      free(defined);
      return nir_verify_inst_err(f, err, err_len, in, i,
                           "unary f64 operand is invalid");
    }
    break;
    case NYIR_COPY:
    case NYIR_I64_TO_F64:
    case NYIR_I64_TO_F32:
    case NYIR_F64_TO_F32:
    case NYIR_F32_TO_F64:
    case NYIR_LOAD_I64:
    case NYIR_VEC4_LOAD_I64:
    case NYIR_VEC8_LOAD_I64:
    case NYIR_VEC4_LOAD_F64:
    case NYIR_VEC8_LOAD_F32:
    case NYIR_VEC4_SET1_F64:
    case NYIR_VEC8_SET1_F32:
    case NYIR_VEC4_SET1_I64:
    case NYIR_VEC4_SHUFFLE_F64:
    case NYIR_VEC8_SHUFFLE_F32:
      if (in->dst < 0 || !nir_value_defined(f, defined, in->a)) {
        free(defined);
        return nir_verify_inst_err(f, err, err_len, in, i, "invalid unary value operand");
      }
      break;
    case NYIR_PHI:
      if (in->dst < 0 || in->phi_incoming_len == 0 || !in->phi_incoming) {
        free(defined);
        return nir_verify_inst_err(f, err, err_len, in, i,
                               "phi requires a destination and incoming edges");
      }
      /*
       * PHIs may read a backedge value defined later in linear NYIR. CFG
       * validation below establishes the edge; require a real definition
       * somewhere rather than a linear predecessor here.
       */
      for (size_t k = 0; k < in->phi_incoming_len; ++k) {
        int v = in->phi_incoming[k].value;
        bool has_definition = false;
        for (size_t j = 0; j < f->len; ++j)
          if (f->data[j].dst == v) { has_definition = true; break; }
        if (!nir_value_valid(f, v) || !has_definition) {
          free(defined);
          return nir_verify_inst_err(f, err, err_len, in, i, "phi has invalid incoming value");
        }
        for (size_t j = 0; j < k; ++j) {
          if (in->phi_incoming[j].predecessor_label ==
              in->phi_incoming[k].predecessor_label) {
            free(defined);
            return nir_verify_inst_err(f, err, err_len, in, i, "phi has duplicate predecessor");
          }
        }
      }
      break;
    case NYIR_LOAD_LOCAL:
      if (in->dst < 0 || in->imm < 0) {
        free(defined);
        return nir_verify_inst_err(f, err, err_len, in, i, "invalid local load");
      }
      break;
    case NYIR_ADDR_LOCAL:
      if (in->dst < 0 || in->imm < 0) {
        free(defined);
        return nir_verify_inst_err(f, err, err_len, in, i, "invalid local address");
      }
      break;
    case NYIR_ADDR_SYMBOL:
      if (in->dst < 0 || !in->symbol || !in->symbol[0]) {
        free(defined);
        return nir_verify_inst_err(f, err, err_len, in, i, "addr.symbol requires a non-empty symbol");
      }
      break;
    case NYIR_ALLOCA:
      if (in->dst < 0 || in->imm < 0) {
        free(defined);
        return nir_verify_inst_err(f, err, err_len, in, i, "alloca requires a valid destination and positive size");
      }
      break;
    case NYIR_COPY_STRUCT:
      if (in->a < 0 || in->b < 0 || in->imm < 0) {
        free(defined);
        return nir_verify_inst_err(f, err, err_len, in, i, "copy.struct requires valid src, dst, and size");
      }
      break;
    case NYIR_CAPTURE_RET:
      if (in->dst < 0 || in->imm < 0 || in->imm > 13 || i == 0 ||
          (f->data[i - 1].op != NYIR_CALL &&
           f->data[i - 1].op != NYIR_CAPTURE_RET)) {
        free(defined);
        return nyir_inst_err(
            err, err_len, in, i,
            "capture.ret requires a call/capture chain immediately before it and selector 0..13");
      }
      break;
    case NYIR_STORE_LOCAL:
      if (in->imm < 0 || !nir_value_defined(f, defined, in->a)) {
        free(defined);
        return nir_verify_inst_err(f, err, err_len, in, i, "invalid local store");
      }
      break;
    case NYIR_STORE_I64:
      if (!nir_value_defined(f, defined, in->a) ||
          !nir_value_defined(f, defined, in->c)) {
        free(defined);
        return nir_verify_inst_err(f, err, err_len, in, i, "invalid memory store");
      }
      break;
    case NYIR_VEC4_STORE_I64:
    case NYIR_VEC8_STORE_I64:
    case NYIR_VEC4_STORE_F64:
    case NYIR_VEC8_STORE_F32:
      if (!nir_value_defined(f, defined, in->a) ||
          !nir_value_defined(f, defined, in->b >= 0 ? in->b : in->c)) {
        free(defined);
        return nir_verify_inst_err(f, err, err_len, in, i,
                               "invalid vector memory store");
      }
      break;
    case NYIR_VEC4_FMA_F64:
    case NYIR_VEC8_FMA_F32:
      if (in->dst < 0 || !nir_value_defined(f, defined, in->a) ||
          !nir_value_defined(f, defined, in->b) ||
          !nir_value_defined(f, defined, in->c)) {
        free(defined);
        return nir_verify_inst_err(f, err, err_len, in, i,
                               "invalid ternary vector operands");
      }
      break;
    case NYIR_RET:
      if (in->a >= 0 && !nir_value_defined(f, defined, in->a)) {
        free(defined);
        return nir_verify_inst_err(f, err, err_len, in, i, "invalid return value");
      }
      break;
    case NYIR_BR:
      if (!nir_label_exists(f, in->imm)) {
        free(defined);
        return nir_verify_inst_err(f, err, err_len, in, i, "missing branch target label");
      }
      break;
    case NYIR_BR_IF:
      if (!nir_value_defined(f, defined, in->a) ||
          !nir_label_exists(f, in->imm)) {
        free(defined);
        return nir_verify_inst_err(f, err, err_len, in, i,
                            "invalid conditional branch operand or target");
      }
      break;
    case NYIR_CALL:
      if (!in->symbol || !in->symbol[0]) {
        free(defined);
        return nir_verify_inst_err(f, err, err_len, in, i, "call has no symbol");
      }
      if (in->imm < 0) {
        free(defined);
        return nir_verify_inst_err(f, err, err_len, in, i, "negative call arg count");
      }
      if (in->imm == 0 &&
          (in->a >= 0 || in->b >= 0 || in->c >= 0 || in->d >= 0 ||
           in->e >= 0 || in->f >= 0)) {
        free(defined);
        return nir_verify_inst_err(f, err, err_len, in, i,
                            "zero-argument call has value operands");
      }
      if (in->imm == 1 &&
          (in->b >= 0 || in->c >= 0 || in->d >= 0 || in->e >= 0 ||
           in->f >= 0)) {
        free(defined);
        return nir_verify_inst_err(f, err, err_len, in, i,
                            "one-argument call has extra value operand");
      }
      if (in->imm == 2 &&
          (in->c >= 0 || in->d >= 0 || in->e >= 0 || in->f >= 0)) {
        free(defined);
        return nir_verify_inst_err(f, err, err_len, in, i,
                            "two-argument call has extra value operand");
      }
      if (in->imm == 3 && (in->d >= 0 || in->e >= 0 || in->f >= 0)) {
        free(defined);
        return nir_verify_inst_err(f, err, err_len, in, i,
                            "three-argument call has extra value operand");
      }
      if (in->imm == 4 && (in->e >= 0 || in->f >= 0)) {
        free(defined);
        return nir_verify_inst_err(f, err, err_len, in, i,
                            "four-argument call has extra value operand");
      }
      if (in->imm == 5 && in->f >= 0) {
        free(defined);
        return nir_verify_inst_err(f, err, err_len, in, i,
                            "five-argument call has extra value operand");
      }
      if (in->imm > 0 && !nir_value_defined(f, defined, in->a)) {
        free(defined);
        return nir_verify_inst_err(f, err, err_len, in, i, "call arg0 is invalid");
      }
      if (in->imm > 1 && !nir_value_defined(f, defined, in->b)) {
        free(defined);
        return nir_verify_inst_err(f, err, err_len, in, i, "call arg1 is invalid");
      }
      if (in->imm > 2 && !nir_value_defined(f, defined, in->c)) {
        free(defined);
        return nir_verify_inst_err(f, err, err_len, in, i, "call arg2 is invalid");
      }
      if (in->imm > 3 && !nir_value_defined(f, defined, in->d)) {
        free(defined);
        return nir_verify_inst_err(f, err, err_len, in, i, "call arg3 is invalid");
      }
      if (in->imm > 4 && !nir_value_defined(f, defined, in->e)) {
        free(defined);
        return nir_verify_inst_err(f, err, err_len, in, i, "call arg4 is invalid");
      }
      if (in->imm > 5 && !nir_value_defined(f, defined, in->f)) {
        free(defined);
        return nir_verify_inst_err(f, err, err_len, in, i, "call arg5 is invalid");
      }
      if (in->imm > NYIR_CALL_MAX_ARGS) {
        free(defined);
        return nir_verify_inst_err(f, err, err_len, in, i,
                            "call exceeds the maximum supported argument count");
      }
      if (in->arg_sizes && in->imm <= 0) {
        free(defined);
        return nir_verify_inst_err(f, err, err_len, in, i,
                               "zero-argument call has aggregate argument metadata");
      }
      if (in->arg_sizes) {
        for (int64_t arg = 0; arg < in->imm; ++arg) {
          uint32_t packed = in->arg_sizes[arg];
          unsigned c0 = NYIR_ARG_AGG_CLASS(packed, 0);
          unsigned c1 = NYIR_ARG_AGG_CLASS(packed, 1);
          if (packed != 0 &&
              (NYIR_ARG_AGG_SIZE(packed) == 0 ||
               c0 > NYIR_ARG_CLASS_AAPCS_INTEGER_A16 ||
               c1 > NYIR_ARG_CLASS_AAPCS_INTEGER_A16)) {
            free(defined);
            return nir_verify_inst_err(f, err, err_len, in, i,
                                   "invalid aggregate argument metadata");
          }
        }
      }
      if (in->imm <= 6) {
        if (in->extra_args || in->extra_args_len != 0) {
          free(defined);
          return nir_verify_inst_err(f, err, err_len, in, i,
                              "call has stray stack-args for a register-only arity");
        }
      } else {
        size_t want = (size_t)(in->imm - 6);
        if (!in->extra_args || in->extra_args_len != want) {
          free(defined);
          return nir_verify_inst_err(f, err, err_len, in, i,
                              "call stack-arg count does not match arity");
        }
        for (size_t k = 0; k < want; ++k) {
          if (!nir_value_defined(f, defined, in->extra_args[k])) {
            free(defined);
            return nir_verify_inst_err(f, err, err_len, in, i, "call stack-arg is invalid");
          }
        }
      }
      break;
    default:
      if (in->op == NYIR_OP_COUNT) {
        free(defined);
        return nir_verify_inst_err(f, err, err_len, in, i, "unknown opcode");
      }
      if (!nir_value_defined(f, defined, in->a) ||
          !nir_value_defined(f, defined, in->b)) {
        free(defined);
        return nir_verify_inst_err(f, err, err_len, in, i, "invalid value operands");
      }
      break;
    }
    if (in->dst >= 0 && defined)
      defined[in->dst] = true;
  }
  free(defined);
  if (!nir_verify_cfg_structure(f, err, err_len))
    return false;
  if (!nir_verify_phi_edges(f, err, err_len))
    return false;
  if (!nir_verify_dominance(f, err, err_len))
    return false;
  if (!nyir_validate_constraints(f, err, err_len))
    return false;
  if (err && err_len > 0)
    err[0] = '\0';
  return true;
}

bool nyir_analyze_binary_fold(nyir_op_t op, int64_t a, int64_t b,
                                       int64_t *out) {
  if (!out)
    return false;
  switch (op) {
  case NYIR_ADD_I64:
    /*
     * Nytrix i64 add/sub/mul wrap.  Spell that in unsigned arithmetic rather
     * than relying on signed C overflow, since this evaluator is shared by
     * constant folding, SCCP, and the NYIR VM.
     */
    *out = (int64_t)((uint64_t)a + (uint64_t)b);
    return true;
  case NYIR_SUB_I64:
    *out = (int64_t)((uint64_t)a - (uint64_t)b);
    return true;
  case NYIR_MUL_I64:
    *out = (int64_t)((uint64_t)a * (uint64_t)b);
    return true;
  case NYIR_DIV_I64:
    if (b == 0 || (a == INT64_MIN && b == -1))
      return false;
    *out = a / b;
    return true;
  case NYIR_MOD_I64:
    if (b == 0 || (a == INT64_MIN && b == -1))
      return false;
    *out = a % b;
    return true;
  case NYIR_AND_I64:
    *out = a & b;
    return true;
  case NYIR_OR_I64:
    *out = a | b;
    return true;
  case NYIR_XOR_I64:
    *out = a ^ b;
    return true;
  case NYIR_SHL_I64:
    if (b < 0 || b >= 64)
      return false;
    *out = (int64_t)((uint64_t)a << (unsigned)b);
    return true;
  case NYIR_SAR_I64:
    if (b < 0 || b >= 64)
      return false;
    *out = a >> (unsigned)b;
    return true;
  case NYIR_ROR_I64: {
    if (b < 0 || b >= 64)
      return false;
    unsigned shift = (unsigned)b;
    if (shift == 0)
      *out = a;
    else
      *out = (int64_t)(((uint64_t)a >> shift) | ((uint64_t)a << (64 - shift)));
    return true;
  }
  default:
    return false;
  }
}

bool nyir_analyze_cmp_fold(nyir_cmp_t cmp, int64_t a, int64_t b,
                                    int64_t *out) {
  switch (cmp) {
  case NYIR_CMP_EQ:
    *out = a == b;
    return true;
  case NYIR_CMP_NE:
    *out = a != b;
    return true;
  case NYIR_CMP_LT:
    *out = a < b;
    return true;
  case NYIR_CMP_LE:
    *out = a <= b;
    return true;
  case NYIR_CMP_GT:
    *out = a > b;
    return true;
  case NYIR_CMP_GE:
    *out = a >= b;
    return true;
  }
  return false;
}

static void nyir_fact_set_const(nyir_value_fact_t *fact, int64_t value) {
  if (!fact)
    return;
  fact->known_const = true;
  fact->const_value = value;
  fact->range.has_min = true;
  fact->range.has_max = true;
  fact->range.min = value;
  fact->range.max = value;
}

static bool nyir_i64_add_checked(int64_t a, int64_t b, int64_t *out) {
  if (!out)
    return false;
  if ((b > 0 && a > INT64_MAX - b) || (b < 0 && a < INT64_MIN - b))
    return false;
  *out = a + b;
  return true;
}

static bool nyir_i64_sub_checked(int64_t a, int64_t b, int64_t *out) {
  if (!out)
    return false;
  if ((b < 0 && a > INT64_MAX + b) || (b > 0 && a < INT64_MIN + b))
    return false;
  *out = a - b;
  return true;
}

static bool nyir_i64_div_checked(int64_t a, int64_t b, int64_t *out) {
  if (!out || b == 0 || (a == INT64_MIN && b == -1))
    return false;
  *out = a / b;
  return true;
}

static bool nyir_i64_mul_checked(int64_t a, int64_t b, int64_t *out) {
#if defined(__GNUC__) || defined(__clang__)
  if (!out)
    return false;
  return !__builtin_mul_overflow(a, b, out);
#else
  if (!out)
    return false;
  if (a == 0 || b == 0) {
    *out = 0;
    return true;
  }
  if (a == -1) {
    if (b == INT64_MIN)
      return false;
    *out = -b;
    return true;
  }
  if (b == -1) {
    if (a == INT64_MIN)
      return false;
    *out = -a;
    return true;
  }
  if (a > 0) {
    if (b > 0) {
      if (a > INT64_MAX / b)
        return false;
    } else if (b < INT64_MIN / a) {
      return false;
    }
  } else {
    if (b > 0) {
      if (a < INT64_MIN / b)
        return false;
    } else if (a != 0 && b < INT64_MAX / a) {
      return false;
    }
  }
  *out = a * b;
  return true;
#endif
}

static bool nyir_range_from_corners(const int64_t *v, size_t n,
                                      nyir_range_t *out) {
  if (!v || n == 0 || !out)
    return false;
  int64_t lo = v[0];
  int64_t hi = v[0];
  for (size_t i = 1; i < n; ++i) {
    if (v[i] < lo)
      lo = v[i];
    if (v[i] > hi)
      hi = v[i];
  }
  *out = (nyir_range_t){.has_min = true, .has_max = true, .min = lo, .max = hi};
  return true;
}

static bool nyir_verify_fact_singleton(const nyir_value_fact_t *fact,
                                       int64_t *value) {
  if (!fact || !value)
    return false;
  if (fact->known_const) {
    *value = fact->const_value;
    return true;
  }
  if (fact->range.has_min && fact->range.has_max &&
      fact->range.min == fact->range.max) {
    *value = fact->range.min;
    return true;
  }
  return false;
}

static bool nyir_fact_binary_range(nyir_op_t op,
                                     const nyir_value_fact_t *a,
                                     const nyir_value_fact_t *b,
                                     nyir_range_t *out) {
  if (!a || !b || !out)
    return false;
  if (op == NYIR_AND_I64) {
    if (b->known_const && b->const_value >= 0) {
      *out = (nyir_range_t){.has_min = true,
                              .has_max = true,
                              .min = 0,
                              .max = b->const_value};
      return true;
    }
    if (a->known_const && a->const_value >= 0) {
      *out = (nyir_range_t){.has_min = true,
                              .has_max = true,
                              .min = 0,
                              .max = a->const_value};
      return true;
    }
  }
  int64_t lo = 0;
  int64_t hi = 0;
  switch (op) {
  case NYIR_ADD_I64: {
    nyir_range_t r = {0};
    if (a->range.has_min && b->range.has_min &&
        nyir_i64_add_checked(a->range.min, b->range.min, &lo)) {
      r.has_min = true;
      r.min = lo;
    }
    if (a->range.has_max && b->range.has_max &&
        nyir_i64_add_checked(a->range.max, b->range.max, &hi)) {
      r.has_max = true;
      r.max = hi;
    }
    if (!r.has_min && !r.has_max)
      return false;
    *out = r;
    return true;
  }
  case NYIR_SUB_I64: {
    nyir_range_t r = {0};
    if (a->range.has_min && b->range.has_max &&
        nyir_i64_sub_checked(a->range.min, b->range.max, &lo)) {
      r.has_min = true;
      r.min = lo;
    }
    if (a->range.has_max && b->range.has_min &&
        nyir_i64_sub_checked(a->range.max, b->range.min, &hi)) {
      r.has_max = true;
      r.max = hi;
    }
    if (!r.has_min && !r.has_max)
      return false;
    *out = r;
    return true;
  }
  case NYIR_MUL_I64: {
    if (!a->range.has_min || !a->range.has_max || !b->range.has_min ||
        !b->range.has_max)
      return false;
    int64_t corners[4];
    if (!nyir_i64_mul_checked(a->range.min, b->range.min, &corners[0]) ||
        !nyir_i64_mul_checked(a->range.min, b->range.max, &corners[1]) ||
        !nyir_i64_mul_checked(a->range.max, b->range.min, &corners[2]) ||
        !nyir_i64_mul_checked(a->range.max, b->range.max, &corners[3]) ||
        !nyir_range_from_corners(corners, 4, out))
      return false;
    return true;
  }
  case NYIR_DIV_I64: {
    if (!a->range.has_min || !a->range.has_max || !b->range.has_min ||
        !b->range.has_max)
      return false;
    if (b->range.min <= 0 && b->range.max >= 0)
      return false;
    int64_t corners[4];
    if (!nyir_i64_div_checked(a->range.min, b->range.min, &corners[0]) ||
        !nyir_i64_div_checked(a->range.min, b->range.max, &corners[1]) ||
        !nyir_i64_div_checked(a->range.max, b->range.min, &corners[2]) ||
        !nyir_i64_div_checked(a->range.max, b->range.max, &corners[3]) ||
        !nyir_range_from_corners(corners, 4, out))
      return false;
    return true;
  }
  case NYIR_MOD_I64: {
    if (!a->range.has_min || !a->range.has_max || !b->range.has_min ||
        !b->range.has_max)
      return false;
    int64_t mod = 0;
    if (!nyir_verify_fact_singleton(b, &mod) || mod == 0 ||
        mod == INT64_MIN)
      return false;
    hi = mod < 0 ? -mod - 1 : mod - 1;
    lo = a->range.min < 0 ? -hi : 0;
    if (a->range.max <= 0)
      hi = 0;
    /*
     * When the non-negative dividend cannot reach the modulus, a % m == a,
     * so its magnitude is bounded by the dividend range rather than |m|-1.
     * Clamp the positive side to a.max so patterns like (x & 7) % 16 keep
     * the tighter [0, 7] fact for downstream BCE instead of widening to 15.
     *
     * a->range.has_min/has_max are already required by the guard above, so
     * a->range.max is a valid bound here.
     */
    if (a->range.min >= 0 && a->range.max < hi)
      hi = a->range.max;
    break;
  }
  case NYIR_SHL_I64: {
    if (!a->range.has_min || !a->range.has_max || !b->range.has_min ||
        !b->range.has_max)
      return false;
    int64_t shift = 0;
    if (!nyir_verify_fact_singleton(b, &shift) || shift < 0 || shift >= 63 ||
        a->range.min < 0)
      return false;
    if (!nyir_i64_mul_checked(a->range.min, (int64_t)1 << shift, &lo) ||
        !nyir_i64_mul_checked(a->range.max, (int64_t)1 << shift, &hi))
      return false;
    break;
  }
  case NYIR_SAR_I64: {
    int64_t shift = 0;
    if (!nyir_verify_fact_singleton(b, &shift) || shift < 0 || shift >= 64)
      return false;
    nyir_range_t r = {0};
    if (a->range.has_min) {
      r.has_min = true;
      r.min = a->range.min >> (unsigned)shift;
    }
    if (a->range.has_max) {
      r.has_max = true;
      r.max = a->range.max >> (unsigned)shift;
    }
    if (!r.has_min && !r.has_max)
      return false;
    *out = r;
    return true;
  }
  case NYIR_AND_I64:
    if (!a->range.has_min || !a->range.has_max || !b->range.has_min ||
        !b->range.has_max)
      return false;
    /*
     * For non-negative operands, a & b cannot exceed either operand, so the
     * tightest sound upper bound from the interval maxima is min(a.max,
     * b.max) rather than the OR-based over-approximation.  This unlocks BCE
     * on masked-index patterns (e.g. arr[i & (len-1)]) that the looser bound
     * would have kept.
     */
    if (a->range.min < 0 || b->range.min < 0)
      return false;
    lo = 0;
    hi = a->range.max < b->range.max ? a->range.max : b->range.max;
    break;
  case NYIR_OR_I64:
  case NYIR_XOR_I64:
    if (!a->range.has_min || !a->range.has_max || !b->range.has_min ||
        !b->range.has_max)
      return false;
    if (a->range.min < 0 || b->range.min < 0)
      return false;
    lo = 0;
    hi = a->range.max | b->range.max;
    break;
  default:
    return false;
  }
  if (lo > hi)
    return false;
  *out = (nyir_range_t){.has_min = true, .has_max = true, .min = lo, .max = hi};
  return true;
}

bool nyir_metadata_summary(const nyir_func_t *f,
                             nyir_metadata_summary_t *summary, char *err,
                             size_t err_len) {
  if (!f)
    return nyir_err(err, err_len, "native NYIR metadata: missing function");
  if (!summary)
    return nyir_err(err, err_len, "native NYIR metadata: missing summary output");
  char verify_err[256] = {0};
  if (!nyir_verify(f, verify_err, sizeof(verify_err)))
    return nyir_err(err, err_len, "native NYIR metadata: verifier rejected input: %s",
                   verify_err);
  memset(summary, 0, sizeof(*summary));
  summary->instructions = f->len;
  summary->values = f->next_value > 0 ? (size_t)f->next_value : 0;
  summary->vectorize_attempted_loops = f->vectorize_attempted_loops;
  summary->vectorize_rejected_loops = f->vectorize_rejected_loops;
  summary->vectorized_loops = f->vectorized_loops;
  for (size_t i = 0; i < f->len; ++i) {
    const nyir_inst_t *in = &f->data[i];
    if (in->op >= 0 && in->op < NYIR_OP_COUNT)
      summary->ops[in->op]++;
    summary->effect_mask |= in->effects | nyir_inst_effects(in);
    if (in->range.has_min || in->range.has_max)
      summary->range_facts++;
    if (in->debug.line || (in->debug.file && in->debug.file[0]))
      summary->debug_locs++;
    switch (in->op) {
    case NYIR_LABEL:
      summary->labels++;
      break;
    case NYIR_BR:
      summary->branches++;
      break;
    case NYIR_BR_IF:
      summary->branches++;
      summary->conditional_branches++;
      break;
    case NYIR_CALL:
      summary->calls++;
      break;
    case NYIR_RET:
      summary->returns++;
      break;
    case NYIR_ADDR_SYMBOL:
      break;
    default:
      break;
    }
  }
  summary->locals = nyir_max_local(f);
  if (err && err_len > 0)
    err[0] = '\0';
  return true;
}

void nyir_metadata_summary_dump(FILE *out, const char *name,
                                  const nyir_metadata_summary_t *summary) {
  if (!out || !summary)
    return;
  fprintf(out,
          "nyir metadata function=%s insts=%zu values=%zu locals=%zu labels=%zu branches=%zu br_if=%zu calls=%zu returns=%zu ranges=%zu debug=%zu vector_attempted=%zu vector_rejected=%zu vectorized=%zu effects=0x%x\n",
          name && name[0] ? name : "<anon>", summary->instructions,
          summary->values, summary->locals, summary->labels,
          summary->branches, summary->conditional_branches, summary->calls,
          summary->returns, summary->range_facts, summary->debug_locs,
          summary->vectorize_attempted_loops,
          summary->vectorize_rejected_loops, summary->vectorized_loops,
          summary->effect_mask);
  for (int op = 0; op < NYIR_OP_COUNT; ++op) {
    if (summary->ops[op])
      fprintf(out, "  op %-14s %zu\n", nyir_op_name((nyir_op_t)op),
              summary->ops[op]);
  }
}

bool nyir_analyze_values(const nyir_func_t *f, nyir_value_fact_t *facts,
                           size_t fact_count, char *err, size_t err_len) {
  if (!f)
    return nyir_err(err, err_len, "native NYIR analysis: missing function");
  if (f->next_value < 0)
    return nyir_err(err, err_len, "native NYIR analysis: invalid value count");
  if ((size_t)f->next_value > fact_count)
    return nyir_err(err, err_len,
                   "native NYIR analysis: fact table too small (%zu < %d)",
                   fact_count, f->next_value);
  if (facts && fact_count > 0)
    memset(facts, 0, fact_count * sizeof(*facts));

  for (size_t i = 0; i < f->len; ++i) {
    const nyir_inst_t *in = &f->data[i];
    if (in->op < 0 || in->op >= NYIR_OP_COUNT)
      return nir_verify_inst_err(f, err, err_len, in, i, "unknown opcode");
    if (in->a >= 0 && facts && (size_t)in->a < fact_count)
      facts[in->a].use_count++;
    if (in->b >= 0 && facts && (size_t)in->b < fact_count)
      facts[in->b].use_count++;
    if (in->op == NYIR_CALL && in->extra_args && facts) {
      for (size_t k = 0; k < in->extra_args_len; ++k) {
        int v = in->extra_args[k];
        if (v >= 0 && (size_t)v < fact_count)
          facts[v].use_count++;
      }
    }
    if (in->op == NYIR_PHI && in->phi_incoming && facts) {
      for (size_t k = 0; k < in->phi_incoming_len; ++k) {
        int v = in->phi_incoming[k].value;
        if (v >= 0 && (size_t)v < fact_count)
          facts[v].use_count++;
      }
    }
    if (in->dst < 0 || !facts || (size_t)in->dst >= fact_count)
      continue;
    nyir_value_fact_t *dst = &facts[in->dst];
    dst->effects |= in->effects | nyir_inst_effects(in);
    if (in->range.has_min || in->range.has_max)
      dst->range = in->range;

    switch (in->op) {
    case NYIR_CONST_I64:
      nyir_fact_set_const(dst, in->imm);
      break;
    case NYIR_COPY:
      if (in->a >= 0 && (size_t)in->a < fact_count) {
        dst->known_const = facts[in->a].known_const;
        dst->const_value = facts[in->a].const_value;
        dst->range = facts[in->a].range;
      }
      break;
    case NYIR_PHI: {
      bool have_input = false;
      bool all_const = true;
      int64_t const_value = 0;
      bool have_min = true;
      bool have_max = true;
      int64_t lo = 0, hi = 0;
      for (size_t k = 0; k < in->phi_incoming_len; ++k) {
        int v = in->phi_incoming[k].value;
        if (v < 0 || (size_t)v >= fact_count) {
          all_const = false;
          have_min = false;
          have_max = false;
          continue;
        }
        const nyir_value_fact_t *input = &facts[v];
        if (!have_input) {
          have_input = true;
          const_value = input->const_value;
          if (input->range.has_min)
            lo = input->range.min;
          else
            have_min = false;
          if (input->range.has_max)
            hi = input->range.max;
          else
            have_max = false;
        } else {
          if (have_min) {
            if (!input->range.has_min)
              have_min = false;
            else if (input->range.min < lo)
              lo = input->range.min;
          }
          if (have_max) {
            if (!input->range.has_max)
              have_max = false;
            else if (input->range.max > hi)
              hi = input->range.max;
          }
        }
        if (!input->known_const || input->const_value != const_value)
          all_const = false;
      }
      if (have_input && all_const)
        nyir_fact_set_const(dst, const_value);
      else if (have_input && (have_min || have_max))
        dst->range = (nyir_range_t){.has_min = have_min, .has_max = have_max,
                                      .min = lo, .max = hi};
      break;
    }
    case NYIR_CMP_I64:
      dst->range.has_min = true;
      dst->range.has_max = true;
      dst->range.min = 0;
      dst->range.max = 1;
      if (in->a >= 0 && in->b >= 0 && (size_t)in->a < fact_count &&
          (size_t)in->b < fact_count && facts[in->a].known_const &&
          facts[in->b].known_const) {
        int64_t folded = 0;
        if (nyir_analyze_cmp_fold(in->cmp, facts[in->a].const_value,
                                    facts[in->b].const_value, &folded))
          nyir_fact_set_const(dst, folded);
      }
      break;
    default:
      if (in->a >= 0 && in->b >= 0 && (size_t)in->a < fact_count &&
          (size_t)in->b < fact_count && facts[in->a].known_const &&
          facts[in->b].known_const) {
        int64_t folded = 0;
        if (nyir_analyze_binary_fold(in->op, facts[in->a].const_value,
                                       facts[in->b].const_value, &folded)) {
          nyir_fact_set_const(dst, folded);
          break;
        }
      }
      if (in->a >= 0 && in->b >= 0 && (size_t)in->a < fact_count &&
          (size_t)in->b < fact_count) {
        nyir_range_t r = {0};
        if (nyir_fact_binary_range(in->op, &facts[in->a], &facts[in->b], &r))
          dst->range = r;
      }
      break;
    }
  }
  if (err && err_len > 0)
    err[0] = '\0';
  return true;
}

bool nyir_validate_constraints(const nyir_func_t *f, char *err,
                                 size_t err_len) {
  if (!f)
    return nyir_err(err, err_len, "native NYIR constraints: missing function");
  bool *known = NULL;
  int64_t *value = NULL;
  if (f->next_value > 0) {
    known = (bool *)calloc((size_t)f->next_value, sizeof(bool));
    value = (int64_t *)calloc((size_t)f->next_value, sizeof(int64_t));
    if (!known || !value) {
      free(known);
      free(value);
      return nyir_err(err, err_len, "native NYIR constraints: out of memory");
    }
  }
  for (size_t i = 0; i < f->len; ++i) {
    const nyir_inst_t *in = &f->data[i];
    if (in->op < 0 || in->op >= NYIR_OP_COUNT) {
      free(known);
      free(value);
      return nir_verify_inst_err(f, err, err_len, in, i, "unknown opcode");
    }
    if (in->range.has_min && in->range.has_max && in->range.min > in->range.max) {
      free(known);
      free(value);
      return nir_verify_inst_err(f, err, err_len, in, i, "range minimum exceeds maximum");
    }
    if ((in->op == NYIR_SHL_I64 || in->op == NYIR_SAR_I64) &&
        in->b >= 0 && known && known[in->b] &&
        (value[in->b] < 0 || value[in->b] >= 64)) {
      free(known);
      free(value);
      return nir_verify_inst_err(f, err, err_len, in, i, "constant shift amount out of range");
    }
    if ((in->op == NYIR_DIV_I64 || in->op == NYIR_MOD_I64) &&
        in->b >= 0 && known && known[in->b] && value[in->b] == 0) {
      free(known);
      free(value);
      return nir_verify_inst_err(f, err, err_len, in, i, "constant divide/modulo by zero");
    }
    if ((in->op == NYIR_DIV_I64 || in->op == NYIR_MOD_I64) &&
        in->a >= 0 && in->b >= 0 && known && known[in->a] && known[in->b] &&
        value[in->a] == INT64_MIN && value[in->b] == -1) {
      free(known);
      free(value);
      return nir_verify_inst_err(f, err, err_len, in, i,
                          "constant signed divide/modulo overflow");
    }
    if (in->dst >= 0 && known) {
      if (in->op == NYIR_CONST_I64) {
        known[in->dst] = true;
        value[in->dst] = in->imm;
      } else if (in->op == NYIR_COPY && in->a >= 0 && known[in->a]) {
        known[in->dst] = true;
        value[in->dst] = value[in->a];
      } else {
        known[in->dst] = false;
      }
    }
  }
  free(known);
  free(value);
  if (err && err_len > 0)
    err[0] = '\0';
  return true;
}
