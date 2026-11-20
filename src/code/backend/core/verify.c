/*
 * NYIR verifier: structural and semantic sanity checks on the IR
 * after each transform pass to catch malformed ops or type mismatches.
 */
#include "base/common.h"
#include "base/compat.h"
#include "code/backend/core/nyr.h"
#include "code/backend/passes/util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool nir_value_valid(const nyr_func_t *f, int v) {
  return v >= 0 && v < f->next_value;
}

static bool nir_aggregate_abi_valid(uint32_t size, unsigned c0, unsigned c1) {
  if (size == 0)
    return c0 == NYIR_ARG_CLASS_NONE && c1 == NYIR_ARG_CLASS_NONE;
  if (size > NYIR_ARG_AGG_SIZE_MASK || c0 > NYIR_ARG_CLASS_AAPCS_INTEGER_A16 ||
      c1 > NYIR_ARG_CLASS_AAPCS_INTEGER_A16)
    return false;
  if (c0 == NYIR_ARG_CLASS_MEMORY)
    return c1 == NYIR_ARG_CLASS_NONE;
  if (c0 == NYIR_ARG_CLASS_HFA_F32)
    return size <= 16 && size % 4 == 0 && c1 == NYIR_ARG_CLASS_NONE;
  if (c0 == NYIR_ARG_CLASS_HFA_F64)
    return size <= 32 && size % 8 == 0 && c1 == NYIR_ARG_CLASS_NONE;
  if (c0 == NYIR_ARG_CLASS_HVA_V128)
    return size <= 64 && size % 16 == 0 && c1 == NYIR_ARG_CLASS_NONE;
  if (size > 16 ||
      (c0 != NYIR_ARG_CLASS_NONE && c0 != NYIR_ARG_CLASS_INTEGER &&
       c0 != NYIR_ARG_CLASS_SSE && c0 != NYIR_ARG_CLASS_AAPCS_INTEGER_A16))
    return false;
  if (c1 != NYIR_ARG_CLASS_NONE && c1 != NYIR_ARG_CLASS_INTEGER &&
      c1 != NYIR_ARG_CLASS_SSE)
    return false;
  if (c0 == NYIR_ARG_CLASS_AAPCS_INTEGER_A16 && c1 != NYIR_ARG_CLASS_NONE &&
      c1 != NYIR_ARG_CLASS_INTEGER)
    return false;
  return true;
}

static bool nir_verify_inst_err(const nyr_func_t *f, char *err, size_t err_len,
                                const nyr_inst_t *in, size_t index,
                                const char *reason) {
  char detail[768];
  const char *why = reason ? reason : "invalid instruction";
  nyr_cfg_t cfg = {0};
  if (f && index < f->len && nyr_cfg_build_topology(f, &cfg) &&
      cfg.inst_block && index < f->len) {
    size_t block = cfg.inst_block[index];
    if (block < cfg.block_count && cfg.block_label &&
        cfg.block_label[block] >= 0)
      snprintf(detail, sizeof(detail), "%s [block=%zu label=L%lld]", why, block,
               (long long)cfg.block_label[block]);
    else
      snprintf(detail, sizeof(detail), "%s [block=%zu]", why, block);
    nyr_cfg_free(&cfg);
    return nyr_inst_err(err, err_len, in, index, detail);
  }
  nyr_cfg_free(&cfg);
  return nyr_inst_err(err, err_len, in, index, why);
}

static bool nir_value_defined(const nyr_func_t *f, const bool *defined, int v) {
  return nir_value_valid(f, v) && defined && defined[v];
}

/*
 * Sorted label table and definition map for the function being verified, so
 * branch, label and phi checks stay linear on functions with many blocks.
 */
typedef struct {
  int64_t imm;
  size_t index;
} nir_verify_label_t;

static _Thread_local const nyr_func_t *nir_verify_func;
static _Thread_local nir_verify_label_t *nir_verify_labels;
static _Thread_local size_t nir_verify_label_count;
/*
 * Last definition, encoded as instruction index + 1; zero means undefined.
 */
static _Thread_local size_t *nir_verify_definitions;

static const nyr_inst_t *nir_verify_def_of(const nyr_func_t *f, int value) {
  if (!f || value < 0 || value >= f->next_value)
    return NULL;
  size_t position = nir_verify_definitions[value];
  return position ? &f->data[position - 1] : NULL;
}

static int nir_verify_label_cmp(const void *a, const void *b) {
  const nir_verify_label_t *x = a, *y = b;
  if (x->imm != y->imm)
    return x->imm < y->imm ? -1 : 1;
  return x->index < y->index ? -1 : x->index > y->index;
}

/*
 * Index of the first definition of a label, or SIZE_MAX.
 */
static size_t nir_verify_first_label(int64_t label) {
  size_t lo = 0, hi = nir_verify_label_count;
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    if (nir_verify_labels[mid].imm < label)
      lo = mid + 1;
    else
      hi = mid;
  }
  return lo < nir_verify_label_count && nir_verify_labels[lo].imm == label
             ? nir_verify_labels[lo].index
             : SIZE_MAX;
}

static bool nir_label_exists(const nyr_func_t *f, int64_t label) {
  if (!f)
    return false;
  if (f == nir_verify_func && nir_verify_labels)
    return nir_verify_first_label(label) != SIZE_MAX;
  for (size_t i = 0; i < f->len; ++i) {
    const nyr_inst_t *in = &f->data[i];
    if (in->op == NYIR_LABEL && in->imm == label)
      return true;
  }
  return false;
}

bool nyr_local_label_symbol_id(const char *symbol, int64_t *label) {
  if (!symbol)
    return false;
  const char *digits;
  static const char local_prefix[] = "__nyc_local_label_";
  if (strncmp(symbol, local_prefix, sizeof(local_prefix) - 1) == 0) {
    digits = symbol + sizeof(local_prefix) - 1;
  } else {
    if (strncmp(symbol, ".Lnyir_", 7) != 0)
      return false;
    const char *suffix = strrchr(symbol, '_');
    if (!suffix || suffix[1] != 'L' || suffix[2] == '\0')
      return false;
    digits = suffix + 2;
  }
  if (!*digits)
    return false;
  uint64_t id = 0;
  for (const unsigned char *p = (const unsigned char *)digits; *p; ++p) {
    if (*p < '0' || *p > '9')
      return false;
    uint64_t digit = (uint64_t)(*p - '0');
    if (id > ((uint64_t)INT64_MAX - digit) / 10)
      return false;
    id = id * 10 + digit;
  }
  if (label)
    *label = (int64_t)id;
  return true;
}

bool nyr_label_referenced(const nyr_func_t *f, int64_t label) {
  if (!f)
    return false;
  for (size_t i = 0; i < f->len; ++i) {
    const nyr_inst_t *in = &f->data[i];
    if (((in->op == NYIR_BR || in->op == NYIR_BR_IF) && in->imm == label) ||
        (in->op == NYIR_INLINE_ASM &&
         (in->imm == -14 || in->imm == -82 || in->imm == -83) &&
         in->c == label))
      return true;
    int64_t addressed_label = -1;
    if (in->op == NYIR_ADDR_SYMBOL &&
        nyr_local_label_symbol_id(in->symbol, &addressed_label) &&
        addressed_label == label)
      return true;
    /*
     * A fall-through block's label may be named only by a successor PHI.
     * Dropping it would merge the block into its predecessor and leave the
     * PHI naming a label that is no longer a CFG predecessor.
     */
    for (size_t k = 0; k < in->phi_incoming_len; ++k)
      if (in->phi_incoming[k].predecessor_label == label)
        return true;
  }
  return false;
}

static unsigned nir_known_effect_mask(void) {
  return NYIR_EFFECT_READ_LOCAL | NYIR_EFFECT_WRITE_LOCAL | NYIR_EFFECT_CALL |
         NYIR_EFFECT_CONTROL | NYIR_EFFECT_READ_MEMORY |
         NYIR_EFFECT_WRITE_MEMORY | NYIR_EFFECT_MAY_TRAP |
         NYIR_EFFECT_VOLATILE | NYIR_EFFECT_ALLOCATION |
         NYIR_EFFECT_UNKNOWN_SIDE_EFFECT | NYIR_EFFECT_IO | NYIR_EFFECT_THREAD |
         NYIR_EFFECT_FFI | NYIR_EFFECT_FENV;
}

/*
 * Instruction flags are part of NYIR's typed ABI metadata.  Keeping their
 * legality here prevents a malformed bundle from silently changing a call or
 * return register class in Machine IR lowering.
 */
static bool nir_flags_valid(const nyr_inst_t *in) {
  if (!in)
    return false;
  unsigned allowed = 0;
  switch (in->op) {
  case NYIR_LOAD_LOCAL:
    allowed = NYIR_INST_F_NARROW32;
    break;
  case NYIR_LABEL:
    allowed = NYIR_INST_F_LABEL_ADDRESS_TAKEN;
    break;
  case NYIR_CALL:
    allowed = NYIR_INST_F_EXTERN | NYIR_INST_F_SRET |
              NYIR_INST_F_EFFECTS_KNOWN | NYIR_INST_F_NOALIAS |
              NYIR_INST_F_INDIRECT | NYIR_INST_F_VARIADIC |
              NYIR_INST_F_I386_REGPARM_MASK | NYIR_INST_F_I386_C_ABI;
    if ((in->flags & NYIR_INST_F_EXTERN) &&
        !(in->flags & NYIR_INST_F_INDIRECT) && in->dst >= 0 && in->imm == 1 &&
        in->symbol &&
        (strcmp(in->symbol, "rt_len") == 0 ||
         strcmp(in->symbol, "rt_value_tag") == 0))
      allowed |= NYIR_INST_F_ABI_META;
    break;
  case NYIR_RET:
    allowed = NYIR_INST_F_I386_RET32 | NYIR_INST_F_I386_SRET_CALLER_POP;
    break;
  case NYIR_INLINE_ASM:
    if (in->imm == -33 || in->imm == -77)
      allowed = NYIR_INST_F_ASM_UNLOCKED;
    break;
  case NYIR_LOAD_I64:
  case NYIR_STORE_I64:
    allowed = NYIR_INST_F_MEM_BYTE | NYIR_INST_F_MEM_U16 | NYIR_INST_F_MEM_I32 |
              NYIR_INST_F_MEM_VOLATILE | NYIR_INST_F_MEM_SEG_FS |
              NYIR_INST_F_MEM_SEG_GS;
    break;
  case NYIR_ADD_I64:
    allowed = NYIR_INST_F_NARROW32 | NYIR_INST_F_NO_OVERFLOW |
              NYIR_INST_F_IN_BOUNDS_ADDR | NYIR_INST_F_AFFINE_ADDR_BASE;
    break;
  case NYIR_DIV_I64:
  case NYIR_MOD_I64:
    allowed = NYIR_INST_F_NARROW32 | NYIR_INST_F_NO_OVERFLOW |
              NYIR_INST_F_DIVIDEND_U31;
    break;
  case NYIR_SHL_I64:
  case NYIR_SAR_I64:
    allowed = NYIR_INST_F_NARROW32;
    break;
  case NYIR_CMP_I64:
    /*
     * The flag is the assume marker; the compare still computes its value.
     */
    allowed = NYIR_INST_F_ASSUME;
    break;
  case NYIR_SUB_I64:
  case NYIR_MUL_I64:
  case NYIR_AND_I64:
  case NYIR_OR_I64:
  case NYIR_XOR_I64:
    /*
     * F_NARROW32 is set by the range-narrowing pass (opt/narrow.c) so the
     * codegen backend may emit 32-bit register forms.
     * NYIR_INST_F_NO_OVERFLOW is set by overflow_check_elim when operations
     * are proven safe by range analysis.
     */
    allowed = NYIR_INST_F_NARROW32 | NYIR_INST_F_NO_OVERFLOW;
    break;
  default:
    return in->flags == 0;
  }
  return (in->flags & ~allowed) == 0;
}

/*
 * PHI operands live on CFG edges, not at their linear instruction position.
 * Keep this check separate from the scalar operand verifier so malformed
 * incoming sets cannot reach SSA destruction or a native backend.
 */
static bool nir_verify_phi_edges(const nyr_func_t *f, char *err,
                                 size_t err_len) {
  bool has_phi = false;
  for (size_t i = 0; i < f->len && !has_phi; ++i)
    has_phi = f->data[i].op == NYIR_PHI;
  if (!has_phi)
    return true;
  nyr_cfg_t cfg = {0};
  if (!nyr_cfg_build_topology(f, &cfg))
    return nyr_err(err, err_len, "native NYIR verify: cannot build CFG");
  for (size_t i = 0; i < f->len; ++i) {
    const nyr_inst_t *phi = &f->data[i];
    if (phi->op != NYIR_PHI)
      continue;
    size_t block = cfg.inst_block[i];
    size_t predecessors = cfg.pred_offsets[block + 1] - cfg.pred_offsets[block];
    if (phi->phi_incoming_len != predecessors) {
      nyr_cfg_free(&cfg);
      return nir_verify_inst_err(
          f, err, err_len, phi, i,
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
        nyr_cfg_free(&cfg);
        return nir_verify_inst_err(
            f, err, err_len, phi, i,
            "phi incoming label is not a CFG predecessor");
      }
    }
  }
  nyr_cfg_free(&cfg);
  return true;
}

static bool nir_verify_cfg_structure(const nyr_func_t *f, char *err,
                                     size_t err_len) {
  nyr_cfg_t cfg = {0};
  if (!nyr_cfg_build_topology(f, &cfg))
    return nyr_err(err, err_len, "native NYIR verify: cannot build CFG");
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
    nyr_op_t op = f->data[end - 1].op;
    if ((op == NYIR_BR || op == NYIR_BR_IF || op == NYIR_RET) &&
        end != cfg.block_end[block]) {
      nyr_cfg_free(&cfg);
      return nir_verify_inst_err(
          f, err, err_len, &f->data[end - 1], end - 1,
          "terminator is followed by a non-NOP instruction");
    }
  }
  nyr_cfg_free(&cfg);
  return true;
}

static bool cfg_dominates_bit_debug(const nyr_cfg_t *cfg, size_t dominator,
                                    size_t dominated) {
  if (!cfg || dominator >= cfg->block_count || dominated >= cfg->block_count)
    return false;
  return nyr_cfg_dominates(cfg, dominator, dominated);
}

static bool nir_value_dominates_use(const nyr_func_t *f, const nyr_cfg_t *cfg,
                                    const int *definitions, int value,
                                    size_t use, char *err, size_t err_len,
                                    const nyr_inst_t *in) {
  if (!nir_value_valid(f, value) || !definitions || definitions[value] < 0) {
    char detail[64];
    snprintf(detail, sizeof(detail), "value v%d has no definition", value);
    return nir_verify_inst_err(f, err, err_len, in, use, detail);
  }
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
      (def_block != use_block && nyr_cfg_dominates(cfg, def_block, use_block)))
    return true;
  if (getenv("NY_DEBUG_DOM")) {
    fprintf(stderr,
            "[dom-debug] def_block=%zu (label=%lld reachable=%d) "
            "use_block=%zu (label=%lld reachable=%d)\n",
            def_block, (long long)cfg->block_label[def_block],
            cfg->reachable[def_block], use_block,
            (long long)cfg->block_label[use_block], cfg->reachable[use_block]);
    for (size_t b = 0; b < cfg->block_count; ++b) {
      fprintf(stderr, "  block %zu label=%lld reachable=%d idom=%d succ=[", b,
              (long long)cfg->block_label[b], cfg->reachable[b],
              cfg->idom ? cfg->idom[b] : -2);
      for (size_t si = cfg->succ_offsets[b]; si < cfg->succ_offsets[b + 1];
           ++si)
        fprintf(stderr, "%zu ", cfg->succ_blocks[si]);
      fprintf(stderr, "] pred=[");
      for (size_t pi = cfg->pred_offsets[b]; pi < cfg->pred_offsets[b + 1];
           ++pi)
        fprintf(stderr, "%zu ", cfg->pred_blocks[pi]);
      fprintf(stderr, "] dominates_use_block=%d\n",
              cfg_dominates_bit_debug(cfg, use_block, b));
    }
  }
  return nir_verify_inst_err(f, err, err_len, in, use,
                             "value definition does not dominate its use");
}

/*
 * A zero-extending load shifted by at least width-1 produces only 0 or 1.
 */
static bool nir_select_shift_is_boolean(const nyr_func_t *f,
                                        const nyr_inst_t *shift,
                                        size_t before) {
  int input = shift->a, count = shift->b;
  unsigned width = 0;
  int64_t amount = -1;
  for (size_t i = before; i-- > 0 && (!width || amount < 0);) {
    const nyr_inst_t *def = &f->data[i];
    if (!width && def->dst == input) {
      if (def->op == NYIR_COPY)
        input = def->a;
      else if (def->op == NYIR_LOAD_I64 && (def->flags & NYIR_INST_F_MEM_BYTE))
        width = 8;
      else if (def->op == NYIR_LOAD_I64 && (def->flags & NYIR_INST_F_MEM_U16))
        width = 16;
      else
        return false;
    }
    if (amount < 0 && def->dst == count) {
      if (def->op == NYIR_COPY)
        count = def->a;
      else if (def->op == NYIR_CONST_I64 && def->imm >= 0 && def->imm < 64)
        amount = def->imm;
      else
        return false;
    }
  }
  return width && amount >= (int64_t)width - 1;
}

static bool nir_select_condition_is_boolean(const nyr_func_t *f, int value,
                                            size_t before, unsigned depth) {
  if (!f || value < 0 || value >= f->next_value || depth > 16)
    return false;
  for (size_t i = before; i-- > 0;) {
    const nyr_inst_t *def = &f->data[i];
    if (def->dst != value)
      continue;
    if (def->op == NYIR_CMP_I64 || def->op == NYIR_CMP_F64 ||
        def->op == NYIR_CMP_F32)
      return true;
    if (def->op == NYIR_CONST_I64)
      return def->imm == 0 || def->imm == 1;
    if (def->op == NYIR_SAR_I64)
      return nir_select_shift_is_boolean(f, def, i);
    /*
     * Bool normalization (`and 1, cmp`): an AND with one 0/1 operand is
     * 0/1, as is OR/XOR of two 0/1 values.
     */
    if (def->op == NYIR_AND_I64)
      return nir_select_condition_is_boolean(f, def->a, i, depth + 1) ||
             nir_select_condition_is_boolean(f, def->b, i, depth + 1);
    if (def->op == NYIR_OR_I64 || def->op == NYIR_XOR_I64)
      return nir_select_condition_is_boolean(f, def->a, i, depth + 1) &&
             nir_select_condition_is_boolean(f, def->b, i, depth + 1);
    return def->op == NYIR_COPY &&
           nir_select_condition_is_boolean(f, def->a, i, depth + 1);
  }
  return false;
}

/*
 * A verification invocation observes one immutable function. Share its lazy
 * type map across the scalar checks; never cache it across IR mutations.
 */
typedef struct {
  nyr_type_map_t types;
  bool ready;
} nir_verify_types_t;

static bool nir_verify_types_get(nir_verify_types_t *shared,
                                 const nyr_func_t *f) {
  if (shared->ready)
    return true;
  if (!nyr_type_map_init(&shared->types, f, nyr_max_local(f)))
    return false;
  shared->ready = true;
  return true;
}

static bool nir_verify_select_types(const nyr_func_t *f,
                                    nir_verify_types_t *shared, char *err,
                                    size_t err_len) {
  bool has_select = false;
  for (size_t i = 0; i < f->len && !has_select; ++i)
    has_select = f->data[i].op == NYIR_SELECT_I64;
  if (!has_select)
    return true;
  if (!nir_verify_types_get(shared, f))
    return nyr_err(err, err_len, "native NYIR verify: cannot build type map");
  nyr_type_map_t types = shared->types;
  for (size_t i = 0; i < f->len; ++i) {
    const nyr_inst_t *in = &f->data[i];
    if (in->op != NYIR_SELECT_I64)
      continue;
    int values[] = {in->dst, in->a, in->b, in->c};
    for (size_t k = 0; k < sizeof(values) / sizeof(values[0]); ++k) {
      int value = values[k];
      if (value < 0 || (size_t)value >= types.value_count ||
          types.value_f64[value] || types.value_f32[value] ||
          types.value_v128_i64[value] || types.value_v256_i64[value] ||
          types.value_v128_f64[value] || types.value_v128_f32[value] ||
          types.value_v256_f64[value]) {
        return nir_verify_inst_err(f, err, err_len, in, i,
                                   "select.i64 requires i64 operands");
      }
    }
  }
  return true;
}

/*
 * Resolve only representation facts carried by NYIR itself. Copies preserve
 * their input class, and a PHI preserves a class only when every incoming
 * value is known and agrees. Do not infer raw/tagged or pointer classes from
 * integer-shaped opcodes.
 */
static nyr_scalar_class_t
nir_verify_value_scalar_class(const nyr_func_t *f, const nyr_type_map_t *types,
                              int value, unsigned depth) {
  if (!f || value < 0 || value >= f->next_value || depth > 32)
    return NYIR_SCALAR_UNKNOWN;
  const nyr_inst_t *def = nir_verify_def_of(f, value);
  if (!def)
    return NYIR_SCALAR_UNKNOWN;
  /*
   * A local load may retain the class of a former occupant of a reused
   * slot. Prefer a concrete same-block reaching store before trusting that
   * stale stamp. Control-flow and call boundaries make the reaching relation
   * ambiguous, so in those cases preserve the instruction's own class.
   */
  if (def->op == NYIR_LOAD_LOCAL) {
    size_t def_at = (size_t)(def - f->data);
    for (size_t j = def_at; j > 0; --j) {
      const nyr_inst_t *prev = &f->data[j - 1];
      if (prev->op == NYIR_STORE_LOCAL && prev->imm == def->imm) {
        if (prev->scalar_class != NYIR_SCALAR_UNKNOWN)
          return prev->scalar_class;
        return nir_verify_value_scalar_class(f, types, prev->a, depth + 1);
      }
      if (prev->op == NYIR_LABEL || prev->op == NYIR_BR ||
          prev->op == NYIR_BR_IF || prev->op == NYIR_CALL ||
          (prev->op == NYIR_ADDR_LOCAL && prev->imm == def->imm))
        break;
    }
  }
  if (def->scalar_class != NYIR_SCALAR_UNKNOWN)
    return def->scalar_class;
  if (def->op == NYIR_COPY)
    return nir_verify_value_scalar_class(f, types, def->a, depth + 1);
  if (def->op == NYIR_PHI && def->phi_incoming_len) {
    nyr_scalar_class_t joined = NYIR_SCALAR_UNKNOWN;
    for (size_t i = 0; i < def->phi_incoming_len; ++i) {
      nyr_scalar_class_t incoming = nir_verify_value_scalar_class(
          f, types, def->phi_incoming[i].value, depth + 1);
      if (incoming == NYIR_SCALAR_UNKNOWN ||
          (joined != NYIR_SCALAR_UNKNOWN && joined != incoming))
        return NYIR_SCALAR_UNKNOWN;
      joined = incoming;
    }
    return joined;
  }
  if (types && (size_t)value < types->value_count) {
    bool f32 = types->value_f32[value];
    bool f64 = types->value_f64[value];
    if (f32 != f64)
      return f64 ? NYIR_SCALAR_F64 : NYIR_SCALAR_F32;
  }
  return NYIR_SCALAR_UNKNOWN;
}

/*
 * Managed language handles, including nominal aggregate handles, can be
 * forwarded into a dynamic-value slot without changing their one-word
 * representation. Raw scalars and tagged values are
 * not interchangeable: boxing or unboxing must appear as an explicit
 * conversion producer before the call. Float width agreement remains checked
 * against the independent type map below.
 */
static bool nir_call_scalar_conversion_supported(nyr_scalar_class_t actual,
                                                 nyr_scalar_class_t formal) {
  if (actual == formal)
    return true;
  if (formal == NYIR_SCALAR_I64_TAGGED &&
      (actual == NYIR_SCALAR_STRING || actual == NYIR_SCALAR_TYPED_BUFFER ||
       actual == NYIR_SCALAR_AGGREGATE || actual == NYIR_SCALAR_OBJECT ||
       actual == NYIR_SCALAR_POINTER || actual == NYIR_SCALAR_CLOSURE))
    return true;
  /*
   * Managed one-word handle classes (str/bytes/typed-buffer/object/closure)
   * and raw address-sized words share one machine word. Crossing between
   * them at a call boundary is a lossless word move in both directions —
   * the raw entry consumes the handle word (rt_bytes_get_raw,
   * rt_color_raw, _dict_store_item via dynamic callers), and typed formals
   * receive raw handles produced by dynamic code. Which conversions must
   * stay explicit is the tagged/raw boxing axis (I64_TAGGED), not the
   * handle-flavor axis. Keep aggregate→pointer one-way (layout vs pointee).
   */
  {
    bool actual_handle =
        actual == NYIR_SCALAR_I64_RAW || actual == NYIR_SCALAR_STRING ||
        actual == NYIR_SCALAR_TYPED_BUFFER || actual == NYIR_SCALAR_OBJECT ||
        actual == NYIR_SCALAR_POINTER || actual == NYIR_SCALAR_CLOSURE;
    bool formal_handle =
        formal == NYIR_SCALAR_I64_RAW || formal == NYIR_SCALAR_STRING ||
        formal == NYIR_SCALAR_TYPED_BUFFER || formal == NYIR_SCALAR_OBJECT ||
        formal == NYIR_SCALAR_POINTER || formal == NYIR_SCALAR_CLOSURE;
    if (actual_handle && formal_handle)
      return true;
  }
  if (actual == NYIR_SCALAR_AGGREGATE && formal == NYIR_SCALAR_POINTER)
    return true;
  /*
   * A typed-buffer value is already an address-sized pointer word. Opaque
   * pointer formals (for example the StringBuilder runtime bridge) consume
   * that address directly without changing its representation. Keep this
   * conversion one-way: an arbitrary pointer is not evidence of a valid
   * typed-buffer handle.
   */
  if (actual == NYIR_SCALAR_TYPED_BUFFER && formal == NYIR_SCALAR_POINTER)
    return true;
  if ((actual == NYIR_SCALAR_I64_RAW && formal == NYIR_SCALAR_POINTER) ||
      (actual == NYIR_SCALAR_POINTER && formal == NYIR_SCALAR_I64_RAW))
    return true;
  return false;
}

static bool nir_verify_call_scalar_classes(const nyr_func_t *f,
                                           nir_verify_types_t *shared,
                                           char *err, size_t err_len) {
  bool has_call = false;
  for (size_t i = 0; i < f->len; ++i)
    has_call |= f->data[i].op == NYIR_CALL;
  if (!has_call)
    return true;

  if (!nir_verify_types_get(shared, f))
    return nyr_err(err, err_len,
                   "native NYIR verify: cannot build call scalar type map");
  nyr_type_map_t types = shared->types;
  for (size_t i = 0; i < f->len; ++i) {
    const nyr_inst_t *in = &f->data[i];
    if (in->op != NYIR_CALL)
      continue;
    int args[NYIR_CALL_MAX_ARGS];
    int argc = 0;
    char call_err[128] = {0};
    if (!nyr_call_args(in, f->next_value, args, NYIR_CALL_MAX_ARGS, &argc,
                       call_err, sizeof(call_err))) {
      return nir_verify_inst_err(
          f, err, err_len, in, i,
          "cannot decode call arguments for scalar check");
    }
    for (int arg = 0; arg < argc; ++arg) {
      if (in->arg_sizes && in->arg_sizes[arg] != 0)
        continue;
      int value = args[arg];
      if (value < 0 || (size_t)value >= types.value_count)
        continue;
      nyr_scalar_class_t actual =
          nir_verify_value_scalar_class(f, &types, value, 0);
      bool actual_f64 =
          actual == NYIR_SCALAR_F64 ||
          (actual == NYIR_SCALAR_UNKNOWN && types.value_f64[value]);
      bool actual_f32 =
          actual == NYIR_SCALAR_F32 ||
          (actual == NYIR_SCALAR_UNKNOWN && types.value_f32[value]);
      nyr_scalar_class_t formal = nyr_call_arg_class_get(in, (size_t)arg);
      bool supported = nir_call_scalar_conversion_supported(actual, formal);
      if (!supported && formal == NYIR_SCALAR_AGGREGATE && in->arg_sizes &&
          (size_t)arg < (size_t)in->imm && in->arg_sizes[arg] == 0 &&
          actual == NYIR_SCALAR_I64_RAW)
        supported = true;
      /*
       * rt_len_strict performs the dynamic type check itself and accepts both
       * canonical tagged sequence values and raw typed-buffer handles. Its
       * call metadata may be specialized to a typed-buffer formal by source
       * inference, but a tagged list/string/dict remains the valid runtime
       * carrier and must reach the checked bridge unchanged.
       */
      if (!supported && in->symbol &&
          (strcmp(in->symbol, "rt_len") == 0 ||
           strcmp(in->symbol, "rt_len_strict") == 0) &&
          actual == NYIR_SCALAR_I64_TAGGED &&
          (formal == NYIR_SCALAR_TYPED_BUFFER || formal == NYIR_SCALAR_OBJECT ||
           formal == NYIR_SCALAR_STRING || formal == NYIR_SCALAR_AGGREGATE))
        supported = true;
      /*
       * This generic sequence dispatcher accepts either a canonical tagged
       * sequence or a raw typed-buffer handle in its value slot. Its NYIR
       * declaration uses the raw word ABI because the index is unboxed; do
       * not confuse that machine-word carrier with a raw scalar payload.
       */
      if (!supported && in->symbol &&
          strcmp(in->symbol, "rt_value_get_index_raw") == 0 && arg == 0 &&
          actual == NYIR_SCALAR_I64_TAGGED && formal == NYIR_SCALAR_I64_RAW)
        supported = true;
      if (actual_f32 && actual_f64) {
        return nir_verify_inst_err(
            f, err, err_len, in, i,
            "call arg producer has conflicting f32 and f64 value facts");
      }
      if (!actual_f32 && !actual_f64 && actual != NYIR_SCALAR_UNKNOWN &&
          formal != NYIR_SCALAR_UNKNOWN && !supported) {
        const nyr_inst_t *producer = NULL;
        size_t producer_at = f->len;
        for (size_t j = 0; j < f->len; ++j) {
          if (f->data[j].dst == value) {
            producer = &f->data[j];
            producer_at = j;
            break;
          }
        }
        char reason[768];
        long long local_slot = producer && producer->op == NYIR_LOAD_LOCAL
                                   ? (long long)producer->imm
                                   : -1;
        size_t last_store_at = f->len;
        int last_store_value = -1;
        nyr_scalar_class_t last_store_class = NYIR_SCALAR_UNKNOWN;
        if (local_slot >= 0) {
          for (size_t j = producer_at; j > 0; --j) {
            const nyr_inst_t *candidate = &f->data[j - 1];
            if (candidate->op == NYIR_STORE_LOCAL &&
                candidate->imm == local_slot) {
              last_store_at = j - 1;
              last_store_value = candidate->a;
              last_store_class = candidate->scalar_class;
              break;
            }
          }
        }
        nyr_scalar_class_t stored_value_class =
            last_store_value >= 0
                ? nir_verify_value_scalar_class(f, &types, last_store_value, 0)
                : NYIR_SCALAR_UNKNOWN;
        snprintf(
            reason, sizeof(reason),
            "call %s arg %d v%d: %s != %s (actual-class=%d formal-class=%d; "
            "from %s@%zu class=%d; slot=%lld last-store@%zu v%d=%s "
            "store-class=%d)",
            in->symbol ? in->symbol : "<indirect>", arg, value,
            nyr_scalar_class_name(actual), nyr_scalar_class_name(formal),
            (int)actual, (int)formal,
            producer ? nyr_op_name(producer->op) : "<missing>", producer_at,
            producer ? (int)producer->scalar_class : (int)NYIR_SCALAR_UNKNOWN,
            local_slot, last_store_at, last_store_value,
            nyr_scalar_class_name(stored_value_class), (int)last_store_class);
        return nir_verify_inst_err(f, err, err_len, in, i, reason);
      }
      if (!actual_f32 && !actual_f64) {
        if (formal == NYIR_SCALAR_F32 || formal == NYIR_SCALAR_F64) {
          size_t producer_at = f->len;
          const nyr_inst_t *producer = NULL;
          for (size_t j = 0; j < f->len; ++j)
            if (f->data[j].dst == value) {
              producer_at = j;
              producer = &f->data[j];
              break;
            }
          char reason[256];
          snprintf(
              reason, sizeof(reason),
              "call %s arg %d (v%d) has float ABI class %s but producer "
              "is not a classified float (producer=%s at inst %zu class=%d)",
              in->symbol ? in->symbol : "<indirect>", arg, value,
              formal == NYIR_SCALAR_F64 ? "f64" : "f32",
              producer ? nyr_op_name(producer->op) : "<missing>", producer_at,
              producer ? (int)producer->scalar_class
                       : (int)NYIR_SCALAR_UNKNOWN);
          return nir_verify_inst_err(f, err, err_len, in, i, reason);
        }
        continue;
      }
      if ((actual_f64 && formal == NYIR_SCALAR_F64) ||
          (actual_f32 && formal == NYIR_SCALAR_F32))
        continue;
      size_t producer_at = f->len;
      const nyr_inst_t *producer = NULL;
      for (size_t j = 0; j < f->len; ++j)
        if (f->data[j].dst == value) {
          producer_at = j;
          producer = &f->data[j];
          break;
        }
      char reason[256];
      snprintf(reason, sizeof(reason),
               "call %s arg %d (v%d) producer is %s via %s at inst %zu "
               "(class=%d) but ABI class is %s",
               in->symbol ? in->symbol : "<indirect>", arg, value,
               actual_f64 ? "f64" : "f32",
               producer ? nyr_op_name(producer->op) : "<missing>", producer_at,
               producer ? (int)producer->scalar_class
                        : (int)NYIR_SCALAR_UNKNOWN,
               formal == NYIR_SCALAR_F64   ? "f64"
               : formal == NYIR_SCALAR_F32 ? "f32"
                                           : "non-float/unknown");
      return nir_verify_inst_err(f, err, err_len, in, i, reason);
    }
  }
  return true;
}

static bool nir_verify_store_scalar_classes(const nyr_func_t *f,
                                            nir_verify_types_t *shared,
                                            char *err, size_t err_len) {
  bool has_store = false;
  for (size_t i = 0; i < f->len; ++i)
    has_store |= f->data[i].op == NYIR_STORE_I64;
  if (!has_store)
    return true;

  if (!nir_verify_types_get(shared, f))
    return nyr_err(err, err_len,
                   "native NYIR verify: cannot build store scalar type map");
  nyr_type_map_t types = shared->types;
  for (size_t i = 0; i < f->len; ++i) {
    const nyr_inst_t *in = &f->data[i];
    if (in->op != NYIR_STORE_I64 || in->c < 0 ||
        (size_t)in->c >= types.value_count)
      continue;
    bool actual_f64 = types.value_f64[in->c];
    bool actual_f32 = types.value_f32[in->c];
    if (actual_f32 && actual_f64) {
      return nir_verify_inst_err(
          f, err, err_len, in, i,
          "store value has conflicting f32 and f64 value facts");
    }
    if ((actual_f64 && in->scalar_class == NYIR_SCALAR_F64) ||
        (actual_f32 && in->scalar_class == NYIR_SCALAR_F32))
      continue;
    if (!actual_f32 && !actual_f64)
      continue;
    char reason[128];
    snprintf(reason, sizeof(reason), "store value is %s but memory class is %s",
             actual_f64 ? "f64" : "f32",
             in->scalar_class == NYIR_SCALAR_F64   ? "f64"
             : in->scalar_class == NYIR_SCALAR_F32 ? "f32"
                                                   : "non-float/unknown");
    return nir_verify_inst_err(f, err, err_len, in, i, reason);
  }
  return true;
}

/*
 * Ownership consistency (C2 ownership-invariant slice): a value's ownership
 * tag must agree through COPY chains, across local stores to the same slot,
 * and across PHI joins. A disagreement means a rewrite dropped or invented a
 * tag. Values with UNKNOWN tags on either side are skipped (unknown is the
 * conservative join).
 */
static bool nir_verify_ownership_consistency(const nyr_func_t *f, char *err,
                                             size_t err_len) {
  for (size_t i = 0; i < f->len; ++i) {
    const nyr_inst_t *in = &f->data[i];
    uint8_t own;
    int other = -1;
    const char *kind = NULL;
    if (in->op == NYIR_COPY && in->a >= 0) {
      own = in->semantic_ownership;
      other = in->a;
      kind = "copy source";
    } else if (in->op == NYIR_PHI) {
      own = in->semantic_ownership;
      kind = "phi join";
      for (size_t k = 0; k < in->phi_incoming_len && other < 0; ++k) {
        int v = in->phi_incoming[k].value;
        const nyr_inst_t *def =
            v >= 0 && v < f->next_value ? nir_verify_def_of(f, v) : NULL;
        if (def && def->semantic_ownership != NY_SEM_OWN_UNKNOWN)
          other = v;
      }
    } else {
      continue;
    }
    if (own == NY_SEM_OWN_UNKNOWN || other < 0)
      continue;
    const nyr_inst_t *odef = nir_verify_def_of(f, other);
    uint8_t oown = odef ? odef->semantic_ownership : NY_SEM_OWN_UNKNOWN;
    if (oown == NY_SEM_OWN_UNKNOWN || oown == own)
      continue;
    char detail[128];
    snprintf(detail, sizeof(detail),
             "%s ownership %u disagrees with producer %u (value v%d)",
             kind ? kind : "value", own, oown, other);
    return nir_verify_inst_err(f, err, err_len, in, i, detail);
  }
  return true;
}

static bool nir_verify_local_store_scalar_classes(const nyr_func_t *f,
                                                  nir_verify_types_t *shared,
                                                  char *err, size_t err_len) {
  bool has_store = false;
  for (size_t i = 0; i < f->len && !has_store; ++i)
    has_store = f->data[i].op == NYIR_STORE_LOCAL;
  if (!has_store)
    return true;
  if (!nir_verify_types_get(shared, f))
    return nyr_err(err, err_len,
                   "native NYIR verify: cannot build local store type map");
  nyr_type_map_t types = shared->types;
  for (size_t i = 0; i < f->len; ++i) {
    const nyr_inst_t *in = &f->data[i];
    if (in->op != NYIR_STORE_LOCAL || in->a < 0 ||
        (size_t)in->a >= types.value_count)
      continue;
    bool actual_f64 = types.value_f64[in->a];
    bool actual_f32 = types.value_f32[in->a];
    if (actual_f64 && actual_f32) {
      return nir_verify_inst_err(
          f, err, err_len, in, i,
          "local store value has conflicting f32 and f64 value facts");
    }
    if ((actual_f64 && in->scalar_class == NYIR_SCALAR_F64) ||
        (actual_f32 && in->scalar_class == NYIR_SCALAR_F32))
      continue;
    if (!actual_f64 && !actual_f32)
      continue;
    char reason[128];
    snprintf(reason, sizeof(reason),
             "local store value is %s but local ABI class is %s",
             actual_f64 ? "f64" : "f32",
             in->scalar_class == NYIR_SCALAR_F64   ? "f64"
             : in->scalar_class == NYIR_SCALAR_F32 ? "f32"
                                                   : "non-float/unknown");
    if (ny_trace_enabled("NY_TRACE_ABI")) {
      for (size_t j = 0; j < f->len; ++j) {
        const nyr_inst_t *other = &f->data[j];
        if (other->op != NYIR_STORE_LOCAL || other->imm != in->imm ||
            other->a < 0 || (size_t)other->a >= types.value_count)
          continue;
        fprintf(stderr,
                "[abi-local-verify] pc=%zu slot=%lld value=v%d class=%d f64=%d "
                "f32=%d\n",
                j, (long long)other->imm, other->a, (int)other->scalar_class,
                (int)types.value_f64[other->a], (int)types.value_f32[other->a]);
      }
    }
    return nir_verify_inst_err(f, err, err_len, in, i, reason);
  }
  return true;
}

/*
 * A call to a runtime panic primitive never returns: it longjmps to the
 * nearest handler or exits.  The cold zero value the lowering emits after it
 * (so the block still ends in a terminator) is unreachable, and its word has
 * no relation to the function's real return ABI — a `-> f64` function's
 * divide-by-zero block must not be forced to return an f64 zero just to
 * satisfy the verifier.
 */
static bool nir_call_never_returns(const nyr_inst_t *in) {
  if (!in || in->op != NYIR_CALL || !in->symbol)
    return false;
  return strcmp(in->symbol, "rt_panic") == 0 ||
         strcmp(in->symbol, "rt_panic_fmt") == 0 ||
         strcmp(in->symbol, "rt_exit") == 0 ||
         strcmp(in->symbol, "rt_process_exit") == 0;
}

static bool nir_verify_return_scalar_classes(const nyr_func_t *f,
                                             nir_verify_types_t *shared,
                                             char *err, size_t err_len) {
  bool has_return = false;
  size_t first_return = SIZE_MAX;
  nyr_scalar_class_t return_abi = NYIR_SCALAR_UNKNOWN;
  bool after_noreturn = false;
  for (size_t i = 0; i < f->len; ++i) {
    const nyr_inst_t *in = &f->data[i];
    if (in->op == NYIR_LABEL) {
      after_noreturn = false;
      continue;
    }
    if (nir_call_never_returns(in)) {
      after_noreturn = true;
      continue;
    }
    if (in->op != NYIR_RET)
      continue;
    if (after_noreturn)
      continue;
    has_return = true;
    /*
     * Aggregate register pieces are governed by the function's aggregate
     * ABI metadata. Scalar words share one integer calling convention even
     * when their source representations differ (nil, tagged values, handles).
     */
    if (f->return_aggregate_size)
      continue;
    nyr_scalar_class_t abi =
        in->scalar_class == NYIR_SCALAR_F64   ? NYIR_SCALAR_F64
        : in->scalar_class == NYIR_SCALAR_F32 ? NYIR_SCALAR_F32
                                              : NYIR_SCALAR_I64_RAW;
    if (first_return != SIZE_MAX && abi != return_abi) {
      char reason[192];
      snprintf(reason, sizeof(reason),
               "function return ABI differs: instruction %zu uses %s, "
               "this return uses %s",
               first_return, nyr_scalar_class_name(return_abi),
               nyr_scalar_class_name(abi));
      return nir_verify_inst_err(f, err, err_len, in, i, reason);
    }
    first_return = first_return == SIZE_MAX ? i : first_return;
    return_abi = abi;
  }
  if (!has_return)
    return true;

  if (!nir_verify_types_get(shared, f))
    return nyr_err(err, err_len,
                   "native NYIR verify: cannot build return scalar type map");
  nyr_type_map_t types = shared->types;
  for (size_t i = 0; i < f->len; ++i) {
    const nyr_inst_t *in = &f->data[i];
    if (in->op != NYIR_RET || in->a < 0 || (size_t)in->a >= types.value_count)
      continue;
    bool actual_f64 = types.value_f64[in->a];
    bool actual_f32 = types.value_f32[in->a];
    nyr_scalar_class_t declared = in->scalar_class;
    bool declared_f64 = declared == NYIR_SCALAR_F64;
    bool declared_f32 = declared == NYIR_SCALAR_F32;
    if (actual_f64 && actual_f32) {
      return nir_verify_inst_err(f, err, err_len, in, i,
                                 "return has conflicting f32/f64 ABI facts");
    }
    if ((actual_f64 && declared_f64) || (actual_f32 && declared_f32))
      continue;
    if (!actual_f64 && !actual_f32 && !declared_f64 && !declared_f32)
      continue;
    char reason[160];
    snprintf(reason, sizeof(reason),
             "return producer is %s but return ABI class is %s",
             actual_f64   ? "f64"
             : actual_f32 ? "f32"
                          : "non-float/unknown",
             declared_f64   ? "f64"
             : declared_f32 ? "f32"
                            : "non-float/unknown");
    return nir_verify_inst_err(f, err, err_len, in, i, reason);
  }
  return true;
}

/*
 * Known ownership annotations on value-preserving edges and stores must not
 * contradict known producer facts. UNKNOWN remains conservative: an edge or
 * store may carry a separately supplied semantic annotation when its producer
 * is unknown. CALL arguments carry both the actual value's source ownership
 * and, independently, the callee formal's borrow/consume disposition. This
 * routine checks actual ownership against the producer; call-contract shape
 * and slot bounds are checked by the call metadata verifier.
 */
static bool nir_verify_ownership_flow(const nyr_func_t *f, char *err,
                                      size_t err_len) {
  size_t value_count = f->next_value > 0 ? (size_t)f->next_value : 0;
  const nyr_inst_t **producer_by_value =
      value_count ? calloc(value_count, sizeof(*producer_by_value)) : NULL;
  if (value_count && !producer_by_value)
    return nyr_err(err, err_len,
                   "native NYIR verify: cannot build ownership value map");
  for (size_t i = 0; i < f->len; ++i) {
    int dst = f->data[i].dst;
    if (dst >= 0 && (size_t)dst < value_count)
      producer_by_value[dst] = &f->data[i];
  }
  for (size_t i = 0; i < f->len; ++i) {
    const nyr_inst_t *in = &f->data[i];
    if (in->op == NYIR_CALL) {
      int args[NYIR_CALL_MAX_ARGS];
      int argc = 0;
      if (!nyr_call_args(in, f->next_value, args, NYIR_CALL_MAX_ARGS, &argc,
                         err, err_len)) {
        free(producer_by_value);
        return false;
      }
      for (int arg = 0; arg < argc; ++arg) {
        uint8_t expected = nyr_call_arg_ownership_get(in, (size_t)arg);
        const nyr_inst_t *producer =
            args[arg] >= 0 && (size_t)args[arg] < value_count
                ? producer_by_value[args[arg]]
                : NULL;
        if (expected != NY_SEM_OWN_UNKNOWN && producer &&
            producer->semantic_ownership != NY_SEM_OWN_UNKNOWN &&
            expected != producer->semantic_ownership) {
          char reason[128];
          snprintf(reason, sizeof(reason),
                   "call argument ownership contradicts its value "
                   "(%u != %u)",
                   (unsigned)expected, (unsigned)producer->semantic_ownership);
          free(producer_by_value);
          return nir_verify_inst_err(f, err, err_len, in, i, reason);
        }
      }
    }
    if ((in->op == NYIR_STORE_LOCAL || in->op == NYIR_STORE_I64) &&
        in->semantic_ownership != NY_SEM_OWN_UNKNOWN) {
      int value = in->op == NYIR_STORE_LOCAL ? in->a : in->c;
      const nyr_inst_t *producer = value >= 0 && (size_t)value < value_count
                                       ? producer_by_value[value]
                                       : NULL;
      if (producer && producer->semantic_ownership != NY_SEM_OWN_UNKNOWN &&
          producer->semantic_ownership != in->semantic_ownership) {
        free(producer_by_value);
        return nir_verify_inst_err(f, err, err_len, in, i,
                                   "store ownership contradicts its value");
      }
    } else if (in->op == NYIR_COPY &&
               in->semantic_ownership != NY_SEM_OWN_UNKNOWN) {
      const nyr_inst_t *producer = in->a >= 0 && (size_t)in->a < value_count
                                       ? producer_by_value[in->a]
                                       : NULL;
      if (producer && producer->semantic_ownership != NY_SEM_OWN_UNKNOWN &&
          producer->semantic_ownership != in->semantic_ownership) {
        free(producer_by_value);
        return nir_verify_inst_err(f, err, err_len, in, i,
                                   "copy ownership contradicts its producer");
      }
    } else if (in->op == NYIR_PHI &&
               in->semantic_ownership != NY_SEM_OWN_UNKNOWN) {
      for (size_t k = 0; k < in->phi_incoming_len; ++k) {
        int value = in->phi_incoming[k].value;
        const nyr_inst_t *producer = value >= 0 && (size_t)value < value_count
                                         ? producer_by_value[value]
                                         : NULL;
        if (producer && producer->semantic_ownership != NY_SEM_OWN_UNKNOWN &&
            producer->semantic_ownership != in->semantic_ownership) {
          free(producer_by_value);
          return nir_verify_inst_err(
              f, err, err_len, in, i,
              "PHI ownership contradicts an incoming value");
        }
      }
    }
  }

  /*
   * Propagate local-slot ownership through the CFG. Each slot carries a set
   * of possible ownership classes; stores kill the prior set, while joins
   * union predecessor facts. UNKNOWN is deliberately non-conflicting.
   */
  bool has_owned_load = false;
  bool has_known_local_store = false;
  bool has_local_address = false;
  for (size_t i = 0; i < f->len; ++i) {
    const nyr_inst_t *in = &f->data[i];
    has_owned_load |= f->data[i].op == NYIR_LOAD_LOCAL &&
                      f->data[i].semantic_ownership != NY_SEM_OWN_UNKNOWN;
    has_local_address |= in->op == NYIR_ADDR_LOCAL;
    if (in->op == NYIR_STORE_LOCAL) {
      uint8_t own = in->semantic_ownership;
      if (own == NY_SEM_OWN_UNKNOWN && in->a >= 0 &&
          (size_t)in->a < value_count && producer_by_value[in->a])
        own = producer_by_value[in->a]->semantic_ownership;
      has_known_local_store |= own != NY_SEM_OWN_UNKNOWN;
    }
  }
  size_t slot_count =
      has_owned_load && has_known_local_store ? nyr_max_local(f) : 0;
  if (slot_count) {
    nyr_cfg_t cfg = {0};
    if (!nyr_cfg_build_topology(f, &cfg)) {
      free(producer_by_value);
      return nyr_err(err, err_len,
                     "native NYIR verify: cannot build ownership CFG");
    }
    if (cfg.block_count && slot_count > SIZE_MAX / cfg.block_count) {
      nyr_cfg_free(&cfg);
      free(producer_by_value);
      return nyr_err(err, err_len,
                     "native NYIR verify: ownership state size overflow");
    }
    size_t state_count = cfg.block_count * slot_count;
    uint8_t *in_state = calloc(state_count, sizeof(*in_state));
    uint8_t *out_state = calloc(state_count, sizeof(*out_state));
    uint8_t *scratch = malloc(slot_count);
    if (state_count && (!in_state || !out_state)) {
      free(in_state);
      free(out_state);
      free(scratch);
      nyr_cfg_free(&cfg);
      free(producer_by_value);
      return nyr_err(err, err_len,
                     "native NYIR verify: cannot allocate ownership states");
    }
    if (!scratch) {
      free(in_state);
      free(out_state);
      nyr_cfg_free(&cfg);
      free(producer_by_value);
      return nyr_err(err, err_len,
                     "native NYIR verify: cannot allocate ownership scratch");
    }
    bool changed;
    do {
      changed = false;
      for (size_t b = 0; b < cfg.block_count; ++b) {
        if (!cfg.reachable[b])
          continue;
        uint8_t *in = &in_state[b * slot_count];
        uint8_t *out = &out_state[b * slot_count];
        if (b == 0)
          for (size_t slot = 0; slot < slot_count; ++slot)
            in[slot] |= (uint8_t)(1u << NY_SEM_OWN_UNKNOWN);
        for (size_t p = cfg.pred_offsets[b]; p < cfg.pred_offsets[b + 1]; ++p) {
          size_t pred = cfg.pred_blocks[p];
          if (!cfg.reachable[pred])
            continue;
          const uint8_t *pred_out = &out_state[pred * slot_count];
          for (size_t slot = 0; slot < slot_count; ++slot)
            in[slot] |= pred_out[slot];
        }
        memcpy(scratch, in, slot_count);
        for (size_t i = cfg.block_start[b]; i < cfg.block_end[b]; ++i) {
          const nyr_inst_t *in_inst = &f->data[i];
          if (in_inst->op != NYIR_STORE_LOCAL || in_inst->imm < 0 ||
              (uint64_t)in_inst->imm >= slot_count) {
            if (has_local_address &&
                (in_inst->op == NYIR_CALL || in_inst->op == NYIR_STORE_I64))
              for (size_t slot = 0; slot < slot_count; ++slot)
                scratch[slot] = (uint8_t)(1u << NY_SEM_OWN_UNKNOWN);
            continue;
          }
          uint8_t own = in_inst->semantic_ownership;
          if (own == NY_SEM_OWN_UNKNOWN && in_inst->a >= 0 &&
              (size_t)in_inst->a < value_count && producer_by_value[in_inst->a])
            own = producer_by_value[in_inst->a]->semantic_ownership;
          scratch[(size_t)in_inst->imm] = (uint8_t)(1u << own);
        }
        if (memcmp(out, scratch, slot_count) != 0) {
          memcpy(out, scratch, slot_count);
          changed = true;
        }
      }
    } while (changed);

    for (size_t b = 0; b < cfg.block_count; ++b) {
      if (!cfg.reachable[b])
        continue;
      uint8_t *state = malloc(slot_count);
      if (!state) {
        free(scratch);
        free(in_state);
        free(out_state);
        nyr_cfg_free(&cfg);
        free(producer_by_value);
        return nyr_err(err, err_len,
                       "native NYIR verify: cannot inspect ownership states");
      }
      memcpy(state, &in_state[b * slot_count], slot_count);
      for (size_t i = cfg.block_start[b]; i < cfg.block_end[b]; ++i) {
        const nyr_inst_t *in = &f->data[i];
        if (in->op == NYIR_LOAD_LOCAL && in->imm >= 0 &&
            (uint64_t)in->imm < slot_count &&
            in->semantic_ownership != NY_SEM_OWN_UNKNOWN) {
          uint8_t possible = state[(size_t)in->imm];
          for (uint8_t own = NY_SEM_OWN_BORROWED; own <= NY_SEM_OWN_SHARED;
               ++own) {
            if ((possible & (uint8_t)(1u << own)) &&
                own != in->semantic_ownership) {
              free(state);
              free(scratch);
              free(in_state);
              free(out_state);
              nyr_cfg_free(&cfg);
              free(producer_by_value);
              return nir_verify_inst_err(
                  f, err, err_len, in, i,
                  "load ownership contradicts a reaching store");
            }
          }
        }
        if (in->op == NYIR_STORE_LOCAL && in->imm >= 0 &&
            (uint64_t)in->imm < slot_count) {
          uint8_t own = in->semantic_ownership;
          if (own == NY_SEM_OWN_UNKNOWN && in->a >= 0 &&
              (size_t)in->a < value_count && producer_by_value[in->a])
            own = producer_by_value[in->a]->semantic_ownership;
          state[(size_t)in->imm] = (uint8_t)(1u << own);
        } else if (has_local_address &&
                   (in->op == NYIR_CALL || in->op == NYIR_STORE_I64)) {
          for (size_t slot = 0; slot < slot_count; ++slot)
            state[slot] = (uint8_t)(1u << NY_SEM_OWN_UNKNOWN);
        }
      }
      free(state);
    }
    free(scratch);
    free(in_state);
    free(out_state);
    nyr_cfg_free(&cfg);
  }
  free(producer_by_value);
  return true;
}

static bool nir_verify_dominance(const nyr_func_t *f, char *err,
                                 size_t err_len) {
  nyr_cfg_t cfg = {0};
  if (!nyr_cfg_build(f, &cfg))
    return nyr_err(err, err_len, "native NYIR verify: cannot build CFG");
  /*
   * value -> defining instruction index, used by the dominance checks
   * below.  NULL when next_value <= 0; the CHECK_DOMINATES paths already
   * tolerate a NULL map (they test definitions[value] < 0 first).  The
   * fill loop previously ran unconditionally outside the next_value > 0
   * guard; the helper now owns both the guard and the dst bounds check.
   */
  int *definitions = nyr_build_defs(f);
  if (f->next_value > 0 && !definitions) {
    nyr_cfg_free(&cfg);
    return nyr_err(err, err_len, "native NYIR verify: out of memory");
  }

#define CHECK_DOMINATES(value)                                                 \
  do {                                                                         \
    if (!nir_value_dominates_use(f, &cfg, definitions, (value), i, err,        \
                                 err_len, in))                                 \
      goto fail;                                                               \
  } while (0)

  for (size_t i = 0; i < f->len; ++i) {
    const nyr_inst_t *in = &f->data[i];
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
            !nyr_cfg_dominates(&cfg, cfg.inst_block[(size_t)definitions[value]],
                               (size_t)pred)) {
          char reason[160];
          int def_at = value >= 0 && value < f->next_value && definitions
                           ? definitions[value]
                           : -1;
          size_t def_block =
              def_at >= 0 ? cfg.inst_block[(size_t)def_at] : SIZE_MAX;
          snprintf(reason, sizeof(reason),
                   "phi value v%d def=i%d block=%zu does not dominate pred=%d "
                   "label=L%lld",
                   value, def_at, def_block, pred,
                   (long long)in->phi_incoming[k].predecessor_label);
          nir_verify_inst_err(f, err, err_len, in, i, reason);
          goto fail;
        }
        (void)phi_block;
      }
      break;
    }
    case NYIR_COPY:
    case NYIR_I64_TO_F64:
    case NYIR_F64_TO_I64:
    case NYIR_F32_TO_I64:
    case NYIR_I64_TO_F32:
    case NYIR_F64_TO_F32:
    case NYIR_F32_TO_F64:
    case NYIR_SQRT_F64:
    case NYIR_SIN_F64:
    case NYIR_COS_F64:
    case NYIR_BSWAP_I64:
    case NYIR_BSWAP32_I64:
    case NYIR_VA_ARG:
    case NYIR_LOAD_I64:
    case NYIR_VEC4_LOAD_I64:
    case NYIR_VEC8_LOAD_I64:
    case NYIR_VEC4_LOAD_F64:
    case NYIR_VEC8_LOAD_F32:
    case NYIR_VEC4_SET1_F64:
    case NYIR_VEC8_SET1_F32:
    case NYIR_VEC4_SET1_I64:
    case NYIR_VEC8_SAR_I64:
    case NYIR_VEC8_SHR_I64:
    case NYIR_VEC8_SET1_I64:
    case NYIR_VEC4_SHUFFLE_F64:
    case NYIR_VEC8_SHUFFLE_F32:
    case NYIR_STORE_LOCAL:
    case NYIR_STACK_GUARD_STORE:
    case NYIR_RET:
    case NYIR_BR_IF:
      if (in->a >= 0)
        CHECK_DOMINATES(in->a);
      if (in->op == NYIR_RET && in->b >= 0)
        CHECK_DOMINATES(in->b);
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
    case NYIR_SELECT_I64:
      CHECK_DOMINATES(in->a);
      CHECK_DOMINATES(in->b);
      CHECK_DOMINATES(in->c);
      break;
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
    case NYIR_INLINE_ASM:
    case NYIR_READ_X86_RSP:
    case NYIR_CONST_I64:
    case NYIR_LABEL:
    case NYIR_LOAD_LOCAL:
    case NYIR_STACK_GUARD_LOAD:
    case NYIR_BR:
    case NYIR_CONST_F64:
    case NYIR_CONST_F32:
    case NYIR_ADDR_LOCAL:
    case NYIR_ADDR_SYMBOL:
    case NYIR_ALLOCA:
    case NYIR_CAPTURE_RET:
    case NYIR_VA_SAVE:
      break;
    case NYIR_BOUNDS_CHECK:
      if (in->a >= 0)
        CHECK_DOMINATES(in->a);
      if (in->b >= 0)
        CHECK_DOMINATES(in->b);
      if (in->c >= 0)
        CHECK_DOMINATES(in->c);
      break;
    default:
      CHECK_DOMINATES(in->a);
      CHECK_DOMINATES(in->b);
      break;
    }
  }
#undef CHECK_DOMINATES
  free(definitions);
  nyr_cfg_free(&cfg);
  return true;
fail:
#undef CHECK_DOMINATES
  free(definitions);
  nyr_cfg_free(&cfg);
  return false;
}

static bool nir_verify_impl(const nyr_func_t *f, char *err, size_t err_len);

bool nyr_ir_verify(const nyr_func_t *f, char *err, size_t err_len) {
  if (!f)
    return nir_verify_impl(f, err, err_len);
  size_t labels = 0;
  for (size_t i = 0; i < f->len; ++i)
    labels += f->data[i].op == NYIR_LABEL;
  nir_verify_label_t *table = malloc((labels + 1) * sizeof(*table));
  size_t *definitions = calloc(f->next_value > 0 ? (size_t)f->next_value : 1,
                               sizeof(*definitions));
  if (!table || !definitions) {
    free(table);
    free(definitions);
    return nyr_err(err, err_len, "native NYIR verify: out of memory");
  }
  size_t count = 0;
  for (size_t i = 0; i < f->len; ++i) {
    const nyr_inst_t *in = &f->data[i];
    if (in->op == NYIR_LABEL)
      table[count++] = (nir_verify_label_t){in->imm, i};
    if (in->dst >= 0 && in->dst < f->next_value)
      definitions[in->dst] = i + 1;
  }
  qsort(table, count, sizeof(*table), nir_verify_label_cmp);
  nir_verify_func = f;
  nir_verify_labels = table;
  nir_verify_label_count = count;
  nir_verify_definitions = definitions;
  bool ok = nir_verify_impl(f, err, err_len);
  nir_verify_func = NULL;
  nir_verify_labels = NULL;
  nir_verify_label_count = 0;
  nir_verify_definitions = NULL;
  free(table);
  free(definitions);
  return ok;
}

static bool nir_verify_impl(const nyr_func_t *f, char *err, size_t err_len) {
  if (!f)
    return nyr_err(err, err_len, "native NYIR verify: missing function");
  if (f->next_value < 0)
    return nyr_err(err, err_len, "native NYIR verify: invalid value count");
  if (f->param_count && !f->param_types)
    return nyr_err(err, err_len, "native NYIR verify: missing parameter types");
  if (f->i386_regparm > 3)
    return nyr_err(err, err_len,
                   "native NYIR verify: invalid i386 regparm count");
  bool has_aggregate_param = false;
  for (size_t i = 0; i < f->param_count; ++i) {
    if (f->param_types[i] > NYIR_PARAM_I8)
      return nyr_err(err, err_len,
                     "native NYIR verify: invalid parameter type");
    if (f->param_types[i] == NYIR_PARAM_AGGREGATE)
      has_aggregate_param = true;
  }
  if (has_aggregate_param && !f->param_abi)
    return nyr_err(err, err_len,
                   "native NYIR verify: aggregate parameter is missing its "
                   "ABI metadata");
  if (f->param_abi && !has_aggregate_param)
    return nyr_err(err, err_len,
                   "native NYIR verify: parameter ABI metadata without an "
                   "aggregate parameter");
  for (size_t i = 0; has_aggregate_param && f->param_abi && i < f->param_count;
       ++i) {
    if (f->param_types[i] != NYIR_PARAM_AGGREGATE)
      continue;
    uint32_t packed = f->param_abi[i];
    uint32_t size = NYIR_ARG_AGG_SIZE(packed);
    unsigned c0 = NYIR_ARG_AGG_CLASS(packed, 0);
    unsigned c1 = NYIR_ARG_AGG_CLASS(packed, 1);
    if (!nir_aggregate_abi_valid(size, c0, c1)) {
      return nyr_err(err, err_len,
                     "native NYIR verify: invalid aggregate parameter ABI "
                     "metadata");
    }
  }
  if (f->return_aggregate_size) {
    uint32_t size = f->return_aggregate_size;
    unsigned c0 = f->return_aggregate_classes[0];
    unsigned c1 = f->return_aggregate_classes[1];
    if (!nir_aggregate_abi_valid(size, c0, c1)) {
      return nyr_err(err, err_len,
                     "native NYIR verify: invalid function aggregate return "
                     "ABI metadata");
    }
  } else if (f->return_aggregate_classes[0] || f->return_aggregate_classes[1]) {
    return nyr_err(err, err_len,
                   "native NYIR verify: aggregate return classes have no "
                   "return size");
  }
  bool *defined = NULL;
  if (f->next_value > 0) {
    defined = (bool *)calloc((size_t)f->next_value, sizeof(bool));
    if (!defined)
      return nyr_err(err, err_len, "native NYIR verify: out of memory");
  }
  for (size_t i = 0; i < f->len; ++i) {
    const nyr_inst_t *in = &f->data[i];
    if (in->op != NYIR_CALL) {
      for (size_t k = 0; k < NYIR_CALL_MAX_ARGS / 16; ++k)
        if (in->arg_scalar_classes[k] != 0) {
          free(defined);
          return nir_verify_inst_err(
              f, err, err_len, in, i,
              "call argument classes on non-call instruction");
        }
      for (size_t k = 0; k < NYIR_CALL_MAX_ARGS / 32; ++k)
        if (in->arg_ownership[k] != 0) {
          free(defined);
          return nir_verify_inst_err(
              f, err, err_len, in, i,
              "call argument ownership on non-call instruction");
        }
      for (size_t k = 0; k < NYIR_CALL_MAX_ARGS / 32; ++k)
        if (in->arg_contracts[k] != 0) {
          free(defined);
          return nir_verify_inst_err(
              f, err, err_len, in, i,
              "call argument contracts on non-call instruction");
        }
    } else {
      for (size_t arg = 0; arg < NYIR_CALL_MAX_ARGS; ++arg) {
        nyr_scalar_class_t cls = nyr_call_arg_class_get(in, arg);
        uint8_t ownership = nyr_call_arg_ownership_get(in, arg);
        uint8_t contract = nyr_call_arg_contract_get(in, arg);
        if (cls > NYIR_SCALAR_AGGREGATE ||
            (arg >= (size_t)(in->imm < 0 ? 0 : in->imm) &&
             cls != NYIR_SCALAR_UNKNOWN) ||
            ownership > NY_SEM_OWN_SHARED ||
            (arg >= (size_t)(in->imm < 0 ? 0 : in->imm) &&
             ownership != NY_SEM_OWN_UNKNOWN) ||
            contract > NYIR_ARG_CONTRACT_RETURNS_BORROW ||
            (arg >= (size_t)(in->imm < 0 ? 0 : in->imm) && contract != 0)) {
          free(defined);
          return nir_verify_inst_err(f, err, err_len, in, i,
                                     "invalid call argument metadata");
        }
      }
    }
    if (in->op != NYIR_CALL && in->arg_sizes) {
      free(defined);
      return nir_verify_inst_err(
          f, err, err_len, in, i,
          "non-call instruction has aggregate argument metadata");
    }
    if (in->op != NYIR_CALL &&
        (in->ret_aggregate_size || in->ret_aggregate_classes[0] ||
         in->ret_aggregate_classes[1])) {
      free(defined);
      return nir_verify_inst_err(
          f, err, err_len, in, i,
          "non-call instruction has aggregate return metadata");
    }
    if (in->op < 0 || in->op >= NYIR_OP_COUNT) {
      free(defined);
      return nir_verify_inst_err(f, err, err_len, in, i, "unknown opcode");
    }
    if (in->scalar_class < NYIR_SCALAR_UNKNOWN ||
        in->scalar_class > NYIR_SCALAR_AGGREGATE ||
        ((in->op == NYIR_LOAD_I64 || in->op == NYIR_STORE_I64) &&
         in->scalar_class == NYIR_SCALAR_F64 &&
         (in->flags & (NYIR_INST_F_MEM_BYTE | NYIR_INST_F_MEM_U16 |
                       NYIR_INST_F_MEM_I32)))) {
      char detail[96];
      free(defined);
      snprintf(detail, sizeof(detail),
               "invalid scalar ABI class %d (flags=0x%x, symbol=%.32s)",
               (int)in->scalar_class, in->flags, in->symbol ? in->symbol : "-");
      return nir_verify_inst_err(f, err, err_len, in, i, detail);
    }
    if (in->semantic_ownership > NY_SEM_OWN_SHARED) {
      free(defined);
      return nir_verify_inst_err(f, err, err_len, in, i,
                                 "invalid semantic ownership class");
    }
    if (in->flags & NYIR_INST_F_NOALIAS) {
      if (in->op != NYIR_CALL || in->dst < 0 || in->imm < 0 ||
          (uint64_t)in->imm > NYIR_CALL_MAX_ARGS) {
        free(defined);
        return nir_verify_inst_err(
            f, err, err_len, in, i,
            "noalias fact requires a result-producing call");
      }
      size_t returns_borrow_count = 0;
      for (size_t arg = 0; arg < (size_t)in->imm; ++arg)
        returns_borrow_count += nyr_call_arg_contract_get(in, arg) ==
                                NYIR_ARG_CONTRACT_RETURNS_BORROW;
      if (returns_borrow_count != 1) {
        free(defined);
        return nir_verify_inst_err(
            f, err, err_len, in, i,
            "noalias fact requires a borrowed result from exactly one "
            "argument");
      }
      if (in->semantic_ownership != NY_SEM_OWN_BORROWED) {
        char detail[128];
        snprintf(detail, sizeof(detail),
                 "noalias call returns a borrow but result ownership is %u",
                 (unsigned)in->semantic_ownership);
        free(defined);
        return nir_verify_inst_err(f, err, err_len, in, i, detail);
      }
    }
    /*
     * Plausibility: control-flow and label instructions carry no value, so
     * an ownership tag on them is a stale stamp from a buggy rewrite.
     */
    if (in->semantic_ownership != NY_SEM_OWN_UNKNOWN &&
        (in->op == NYIR_NOP || in->op == NYIR_LABEL || in->op == NYIR_BR ||
         in->op == NYIR_BR_IF)) {
      free(defined);
      char detail[96];
      snprintf(detail, sizeof(detail),
               "control-flow op %s carries ownership tag %u",
               nyr_op_name(in->op), (unsigned)in->semantic_ownership);
      return nir_verify_inst_err(f, err, err_len, in, i, detail);
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
      return nir_verify_inst_err(f, err, err_len, in, i,
                                 "unknown comparison predicate");
    }
    unsigned required_effects = nyr_inst_effects(in);
    unsigned known_effects = nir_known_effect_mask();
    if ((in->effects & ~known_effects) != 0) {
      free(defined);
      return nir_verify_inst_err(f, err, err_len, in, i, "invalid effect mask");
    }
    bool trusted_call_ok = in->op == NYIR_CALL &&
                           (in->flags & NYIR_INST_F_EFFECTS_KNOWN) &&
                           (in->effects & NYIR_EFFECT_CALL) != 0;
    if (in->effects != required_effects && !trusted_call_ok) {
      unsigned allowed = required_effects;
      if ((in->effects &
           (NYIR_EFFECT_READ_MEMORY | NYIR_EFFECT_WRITE_MEMORY)) ||
          in->op == NYIR_LOAD_I64 || in->op == NYIR_STORE_I64)
        allowed |= NYIR_EFFECT_VOLATILE;
      if (in->effects != allowed) {
        char detail[192];
        snprintf(detail, sizeof(detail),
                 "effect mask does not match opcode effects"
                 " (actual=0x%x required=0x%x symbol=%s flags=0x%x)",
                 in->effects, required_effects,
                 in->symbol ? in->symbol : "<none>", in->flags);
        free(defined);
        return nir_verify_inst_err(f, err, err_len, in, i, detail);
      }
    }
    if ((in->range.has_min && !in->range.has_max) ||
        (!in->range.has_min && in->range.has_max)) {
      free(defined);
      return nir_verify_inst_err(f, err, err_len, in, i,
                                 "incomplete range fact");
    }
    if (in->range.has_min && in->range.has_max &&
        in->range.min > in->range.max) {
      free(defined);
      return nir_verify_inst_err(f, err, err_len, in, i, "invalid range fact");
    }
    if (in->dst < 0 && (in->range.has_min || in->range.has_max)) {
      free(defined);
      return nir_verify_inst_err(
          f, err, err_len, in, i,
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
      return nir_verify_inst_err(f, err, err_len, in, i,
                                 "invalid debug location");
    }
    if (in->dst >= f->next_value) {
      free(defined);
      return nir_verify_inst_err(f, err, err_len, in, i,
                                 "invalid destination value");
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
    case NYIR_INLINE_ASM: {
      if (in->imm == -196) {
        bool address =
            in->a == -1 ? in->symbol && in->symbol[0]
                        : !in->symbol && nir_value_defined(f, defined, in->a);
        if (in->dst < 0 || !address || in->b != -1 || in->c != -1 ||
            in->d != -1 || in->e != -1 || in->f != -1 || in->extra_args_len) {
          free(defined);
          return nir_verify_inst_err(f, err, err_len, in, i,
                                     "invalid RIP-relative asm operand");
        }
        break;
      }
      if (in->imm == -195) {
        bool valid = in->dst == -1 && in->symbol && !in->extra_args_len &&
                     strcmp(in->symbol, "es_stosw_stosl") == 0;
        const int operands[] = {in->a, in->b, in->c, in->d, in->e, in->f};
        for (size_t operand = 0; operand < 6; ++operand)
          valid &= nir_value_defined(f, defined, operands[operand]);
        if (!valid) {
          free(defined);
          return nir_verify_inst_err(f, err, err_len, in, i,
                                     "invalid ES string-store operands");
        }
        break;
      }
      if (in->imm == -194) {
        if (in->dst != -1 || !nir_value_defined(f, defined, in->a) ||
            in->b != -1 || in->c != -1 || in->d != -1 || in->e != -1 ||
            in->f != -1 || in->extra_args_len || !in->symbol ||
            strcmp(in->symbol, "lcallw") != 0) {
          free(defined);
          return nir_verify_inst_err(f, err, err_len, in, i,
                                     "invalid far-call memory operand");
        }
        break;
      }
      if (in->imm == -108 || in->imm == -193) {
        static const char *const symbols[] = {"fs8", "fs16", "fs32",
                                              "gs8", "gs16", "gs32"};
        bool known_symbol = false;
        if (in->symbol)
          for (size_t symbol = 0; symbol < 6; ++symbol)
            known_symbol |= strcmp(in->symbol, symbols[symbol]) == 0;
        bool read = in->imm == -108;
        bool valid =
            known_symbol && nir_value_defined(f, defined, in->a) &&
            (read ? in->dst >= 0 && in->dst < f->next_value && in->b == -1
                  : in->dst == -1 && nir_value_defined(f, defined, in->b)) &&
            in->c == -1 && in->d == -1 && in->e == -1 && in->f == -1 &&
            !in->extra_args_len;
        if (!valid) {
          free(defined);
          return nir_verify_inst_err(f, err, err_len, in, i,
                                     "invalid segmented memory operands");
        }
        break;
      }
      if (in->imm == -192) {
        if (in->dst != -1 || !nir_value_defined(f, defined, in->a) ||
            in->b != -1 || in->c != -1 || in->d != -1 || in->e != -1 ||
            in->f != -1 || in->extra_args_len || !in->symbol ||
            strcmp(in->symbol, "mov_r9") != 0) {
          free(defined);
          return nir_verify_inst_err(f, err, err_len, in, i,
                                     "invalid fixed-register R9 input");
        }
        break;
      }
      if (in->imm == -191) {
        bool valid = in->dst == -1 && in->symbol && !in->extra_args_len &&
                     (strcmp(in->symbol, "rep_movsq_byte_tail") == 0 ||
                      strcmp(in->symbol, "rep_movsl_byte_tail") == 0);
        const int operands[] = {in->a, in->b, in->c, in->d, in->e, in->f};
        for (size_t operand = 0; operand < 6; ++operand)
          valid &= nir_value_defined(f, defined, operands[operand]);
        if (!valid) {
          free(defined);
          return nir_verify_inst_err(
              f, err, err_len, in, i,
              "invalid repeated copy byte-tail operands");
        }
        break;
      }
      if (in->imm == -190) {
        bool valid = in->dst == -1 && in->f == -1 && in->symbol &&
                     strcmp(in->symbol, "divl_u32") == 0 && !in->extra_args_len;
        const int operands[] = {in->a, in->b, in->c, in->d, in->e};
        for (size_t operand = 0; operand < 5; ++operand)
          valid &= nir_value_defined(f, defined, operands[operand]);
        if (!valid) {
          free(defined);
          return nir_verify_inst_err(
              f, err, err_len, in, i,
              "invalid unsigned 64-by-32 divide operands");
        }
        break;
      }
      if (in->imm == -189) {
        bool valid = in->dst >= 0 && in->dst < f->next_value && in->symbol &&
                     strcmp(in->symbol, "repe_cmpsb") == 0 &&
                     !in->extra_args_len;
        const int operands[] = {in->a, in->b, in->c, in->d, in->e, in->f};
        for (size_t operand = 0; operand < 6; ++operand)
          valid &= nir_value_defined(f, defined, operands[operand]);
        if (!valid) {
          free(defined);
          return nir_verify_inst_err(f, err, err_len, in, i,
                                     "invalid repeated CMPSB operands");
        }
        break;
      }
      if (in->imm == -187 || in->imm == -188) {
        if (in->dst != -1 || !nir_value_defined(f, defined, in->a) ||
            !nir_value_defined(f, defined, in->b) ||
            !nir_value_defined(f, defined, in->c) || in->d != -1 ||
            in->e != -1 || in->f != -1 || in->extra_args_len || !in->symbol ||
            strcmp(in->symbol, in->imm == -187 ? "rdmsr" : "wrmsr") != 0) {
          free(defined);
          return nir_verify_inst_err(f, err, err_len, in, i,
                                     "invalid raw MSR operands");
        }
        break;
      }
      if (in->imm == -142) {
        int slot = -1;
        int consumed = -1;
        bool valid = in->dst == -1 && in->a >= 0 && in->f == -1 &&
                     in->extra_args_len == 0 && in->symbol &&
                     sscanf(in->symbol, "kvm_vmmcall;slot=%d%n", &slot,
                            &consumed) == 1 &&
                     consumed >= 0 && slot >= 0 &&
                     (in->symbol[consumed] == '\0' ||
                      strcmp(in->symbol + consumed, ";alt=271") == 0);
        const int inputs[] = {in->a, in->b, in->c, in->d, in->e};
        bool ended = false;
        for (size_t arg = 0; arg < 5; ++arg) {
          if (inputs[arg] < 0) {
            ended = true;
            continue;
          }
          valid = valid && !ended && inputs[arg] < f->next_value &&
                  defined[inputs[arg]];
        }
        if (!valid) {
          free(defined);
          return nir_verify_inst_err(f, err, err_len, in, i,
                                     "invalid KVM vmmcall");
        }
        break;
      }
      if (in->imm == -140 || in->imm == -141) {
        int consumed = -1;
        unsigned port = 0;
        bool valid = in->dst == -1 && in->extra_args_len == 6 &&
                     in->extra_args && in->symbol &&
                     (in->imm == -141
                          ? (sscanf(in->symbol, "vmware_port;port=%u%n", &port,
                                    &consumed) == 1 &&
                             consumed >= 0 && port <= UINT16_MAX &&
                             (in->symbol[consumed] == '\0' ||
                              strcmp(in->symbol + consumed, ";alt=274") == 0))
                          : (strcmp(in->symbol, "vmcall") == 0 ||
                             strcmp(in->symbol, "vmmcall") == 0));
        const int inputs[] = {in->a, in->b, in->c, in->d, in->e, in->f};
        for (size_t arg = 0; valid && arg < 6; ++arg)
          valid = nir_value_defined(f, defined, inputs[arg]) &&
                  nir_value_defined(f, defined, in->extra_args[arg]);
        if (!valid) {
          free(defined);
          return nir_verify_inst_err(f, err, err_len, in, i,
                                     "invalid VMware fixed-register call");
        }
        break;
      }
      bool builtin_result =
          in->symbol &&
          (strcmp(in->symbol, "__nyc_builtin_return_address_0") == 0 ||
           strcmp(in->symbol, "__nyc_builtin_frame_address_0") == 0 ||
           strcmp(in->symbol, "__nyc_builtin_alloca_dynamic") == 0);
      bool port_output_asm = in->imm >= -54 && in->imm <= -52;
      bool port_input_asm = in->imm >= -57 && in->imm <= -55;
      bool memory_load32_asm = in->imm == -58 || in->imm == -101;
      bool data_segment_reload_asm = in->imm == -102;
      bool iret_to_self_asm = in->imm == -59;
      bool control_register_write_asm = in->imm >= -64 && in->imm <= -60;
      bool control_register_read_asm = in->imm >= -69 && in->imm <= -65;
      bool control_register_asm =
          control_register_write_asm || control_register_read_asm;
      bool fs_segment_load_asm = in->imm == -70;
      bool typed_segment_load_asm = in->imm == -79;
      bool descriptor_store_asm =
          in->imm == -71 || in->imm == -72 || in->imm == -152;
      bool descriptor_load_asm = in->imm == -73 || in->imm == -74;
      bool descriptor_table_asm = descriptor_store_asm || descriptor_load_asm;
      bool task_register_load_asm = in->imm == -75 || in->imm == -76;
      bool random_word_asm = in->imm == -132;
      bool checked_segment_load_asm = in->imm == -134;
      bool repeat_movsb_asm = in->imm == -135;
      bool repeat_movsl_tail_asm = in->imm == -177;
      bool xstore_asm = in->imm == -178;
      bool movntdqa_asm = in->imm == -179;
      bool crc32_asm = in->imm == -180;
      bool repeat_stosb_asm = in->imm == -136;
      bool delay_loop_asm = in->imm == -167;
      bool usercopy_movnti_asm = in->imm == -168;
      bool alt_reloc_selftest_asm = in->imm == -137;
      bool repeat_movs_alternative_asm = in->imm == -138;
      bool invlpg_asm = in->imm == -139;
      bool far_jump_asm = in->imm == -146;
      bool indirect_jump_asm = in->imm == -157;
      bool gs_percpu_add_asm = in->imm == -181;
      bool apic_alt_io_asm = in->imm == -147;
      bool kexec_segment_load_asm = in->imm == -148;
      bool runtime_const_shift_asm = in->imm == -149;
      bool runtime_ptr_clamp_asm = in->imm == -156;
      bool cmpxchg16b_asm = in->imm == -160;
      bool ip_fast_csum_asm = in->imm == -176;
      bool futex_cmpxchg_asm = in->imm == -162;
      bool futex_runtime_mask_asm = in->imm == -163;
      bool futex_xchg_asm = in->imm == -164;
      bool futex_xadd_asm = in->imm == -165;
      bool futex_atomic_op2_asm = in->imm == -166;
      bool register_output =
          in->imm == -1 || in->imm == -9 || in->imm == -10 || in->imm == -11 ||
          in->imm == -12 || in->imm == -13 || in->imm == -155 ||
          in->imm == -145 || in->imm == -147 || in->imm == -16 ||
          in->imm == -17 || in->imm == -18 || in->imm == -19 ||
          in->imm == -20 || in->imm == -21 || in->imm == -22 ||
          in->imm == -23 || in->imm == -25 || in->imm == -47 ||
          in->imm == -107 || in->imm == -110 || in->imm == -123 ||
          in->imm == -124 || in->imm == -129 || in->imm == -130 ||
          in->imm == -131 || in->imm == -132 || in->imm == -167 ||
          in->imm == -50 || in->imm == -80 || in->imm == -81 ||
          in->imm == -105 || in->imm == -106 || in->imm == -150 ||
          in->imm == -151 || runtime_const_shift_asm || runtime_ptr_clamp_asm ||
          cmpxchg16b_asm || ip_fast_csum_asm || port_input_asm ||
          memory_load32_asm || control_register_read_asm ||
          (in->imm >= -46 && in->imm <= -40);
      bool read_write_xchg = in->imm >= -46 && in->imm <= -40;
      bool read_write_xadd = in->imm == -50;
      bool syscall_asm = in->imm == -20;
      bool syscall_i386_asm = in->imm == -105;
      bool divq_u64_asm = in->imm == -106;
      bool rdpkru_asm = in->imm == -107;
      bool lar_valid_ss_asm = in->imm == -110;
      bool verw_asm = in->imm == -114;
      bool clflush_asm = in->imm == -115;
      bool monitor_asm = in->imm == -116;
      bool fxsave_asm = in->imm == -117;
      bool fninit_status_asm = in->imm == -118;
      bool fdiv_bug_asm = in->imm == -119;
      bool fildl_clear_asm = in->imm == -120;
      bool ldmxcsr_asm = in->imm == -121;
      bool fxrstor_extable_asm = in->imm == -122;
      bool fxrstor_error_asm = in->imm == -123;
      bool fxrstor_user_asm = in->imm == -124;
      bool xgetbv_asm = in->imm == -125;
      bool xsave_alternative_asm = in->imm == -126;
      bool xrstor_alternative_asm = in->imm == -127;
      bool fnsave_user_asm = in->imm == -129;
      bool put_user_nocheck_asm = in->imm == -130;
      bool xsave_user_asm = in->imm == -131;
      bool wrfsbase_asm = in->imm == -109;
      bool irq_stack_switch_asm = in->imm == -111;
      bool call_on_stack_asm = in->imm == -153;
      bool unsigned_arithmetic_asm =
          in->imm == -21 || in->imm == -22 || in->imm == -23;
      bool asm_goto = in->imm == -14 || in->imm == -82 || in->imm == -83;
      bool faultable_store_goto = in->imm == -82;
      bool faultable_load_goto = in->imm == -83;
      bool faultable_load_reg = in->imm == -84;
      bool atomic_inc = in->imm == -15;
      bool atomic_bitwise = in->imm == -33;
      bool atomic_bitwise64 = in->imm == -77;
      bool atomic_bitwise_condition = in->imm == -47;
      bool atomic_arithmetic_condition = in->imm == -81;
      bool get_user_asm = in->imm == -24;
      bool put_user_asm = in->imm == -34;
      char get_user_helper[64] = {0};
      int get_user_return_slot = -1;
      int get_user_value_slot = -1;
      bool get_user_symbol_valid =
          in->symbol &&
          sscanf(in->symbol, "call %63[^;];nyc_slots=%d,%d", get_user_helper,
                 &get_user_return_slot, &get_user_value_slot) == 3 &&
          get_user_return_slot >= 0 && get_user_value_slot >= 0 &&
          (strcmp(get_user_helper, "__get_user_1") == 0 ||
           strcmp(get_user_helper, "__get_user_2") == 0 ||
           strcmp(get_user_helper, "__get_user_4") == 0 ||
           strcmp(get_user_helper, "__get_user_8") == 0 ||
           strcmp(get_user_helper, "__get_user_nocheck_1") == 0 ||
           strcmp(get_user_helper, "__get_user_nocheck_2") == 0 ||
           strcmp(get_user_helper, "__get_user_nocheck_4") == 0 ||
           strcmp(get_user_helper, "__get_user_nocheck_8") == 0);
      if (ip_fast_csum_asm) {
        bool valid = in->dst >= 0 && in->dst < f->next_value && in->a >= 0 &&
                     in->a < f->next_value && defined[in->a] && in->b >= 0 &&
                     in->b < f->next_value && defined[in->b] && in->c == -1 &&
                     in->d == -1 && in->e == -1 && in->f == -1 &&
                     in->extra_args_len == 0 && in->phi_incoming_len == 0 &&
                     in->symbol && strcmp(in->symbol, "ip_fast_csum") == 0 &&
                     (in->effects & NYIR_EFFECT_UNKNOWN_SIDE_EFFECT) != 0 &&
                     (in->effects & NYIR_EFFECT_VOLATILE) != 0;
        if (!valid) {
          free(defined);
          return nir_verify_inst_err(f, err, err_len, in, i,
                                     "invalid ip_fast_csum operands");
        }
        break;
      }
      bool runtime_shift_symbol_valid =
          runtime_const_shift_asm && in->symbol && in->symbol[0] &&
          !(in->symbol[0] >= '0' && in->symbol[0] <= '9') &&
          strspn(in->symbol, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVW"
                             "XYZ0123456789_") == strlen(in->symbol);
      bool bsr_asm = in->imm == -25;
      bool rdmsr_asm = in->imm == -27;
      bool rdmsr_safe_asm = in->imm == -28;
      bool rdmsr_safe_i386_asm = in->imm == -103;
      bool rdpmc_i386_asm = in->imm == -104;
      bool wrmsr_safe_asm = in->imm == -29;
      bool alternative_wrmsr_asm = in->imm == -100;
      bool rdpmc_asm = in->imm == -30;
      bool cpuid_asm = in->imm == -31;
      bool debug_register_write = in->imm == -32;
      bool lkgs_asm = in->imm == -78;
      bool x86_rip_capture_asm = in->imm == -150;
      bool x86_rbp_capture_asm = in->imm == -151;
      bool register_memory_input_output = in->imm == -9;
      bool fixed_register_input = in->imm == -5;
      bool memory_register_input =
          in->imm == -6 || in->imm == -7 || in->imm == -8 || in->imm == -154;
      bool memory_register_output = in->imm == -7;
      bool condition_output =
          in->imm == -4 || in->imm == -8 || atomic_bitwise_condition;
      bool memory_output =
          in->imm == -2 || in->imm == -3 || in->imm == -8 || in->imm == -4 ||
          in->imm == -10 || in->imm == -11 || in->imm == -12 ||
          in->imm == -155 || in->imm == -145 || in->imm == -16 ||
          atomic_bitwise_condition || read_write_xchg || read_write_xadd ||
          descriptor_store_asm || atomic_bitwise64;
      bool asm_goto_target = false;
      if (asm_goto)
        asm_goto_target = nir_label_exists(f, in->c);
      if (call_on_stack_asm) {
        bool valid = in->dst == -1 && in->a >= 0 && in->a < f->next_value &&
                     defined[in->a] && in->b >= -1 && in->c >= -1 &&
                     in->d >= -1 &&
                     (in->b < 0 || (in->b < f->next_value && defined[in->b])) &&
                     (in->c < 0 || (in->c < f->next_value && defined[in->c])) &&
                     (in->d < 0 || (in->d < f->next_value && defined[in->d])) &&
                     (in->c < 0 || in->b >= 0) && (in->d < 0 || in->c >= 0) &&
                     in->e == -1 && in->f == -1 && in->extra_args_len == 0 &&
                     in->symbol && in->symbol[0] &&
                     (in->effects & NYIR_EFFECT_CALL) != 0;
        if (!valid) {
          free(defined);
          return nir_verify_inst_err(f, err, err_len, in, i,
                                     "invalid call_on_stack operands");
        }
        break;
      }
      if (runtime_ptr_clamp_asm &&
          (in->dst < 0 || in->a < 0 || in->a >= f->next_value ||
           !defined[in->a] || in->b < 0 || in->b >= f->next_value ||
           !defined[in->b] || in->c != -1 || in->d != -1 || in->e != -1 ||
           in->f != -1 || !in->symbol ||
           strcmp(in->symbol, "runtime_ptr_clamp") != 0 ||
           in->extra_args_len != 0 || in->phi_incoming_len != 0)) {
        free(defined);
        return nir_verify_inst_err(f, err, err_len, in, i,
                                   "invalid runtime pointer clamp operands");
      }
      if (runtime_ptr_clamp_asm &&
          ((in->dst >= 0) != (builtin_result || register_output ||
                              condition_output || memory_register_output ||
                              faultable_load_goto || faultable_load_reg))) {
        free(defined);
        return nir_verify_inst_err(f, err, err_len, in, i,
                                   "invalid runtime pointer clamp result");
      }
      if (runtime_ptr_clamp_asm)
        break;
      if (in->imm == -186) {
        static const char *const names[] = {"rep outsb", "rep outsw",
                                            "rep outsl", "rep insb",
                                            "rep insw",  "rep insl"};
        bool named = false;
        for (size_t form = 0;
             in->symbol && form < sizeof(names) / sizeof(*names); ++form)
          named |= strcmp(in->symbol, names[form]) == 0;
        bool valid = named && in->dst == -1 && in->d == -1 && in->e == -1 &&
                     in->f == -1 && !in->extra_args_len &&
                     !in->phi_incoming_len;
        const int inputs[] = {in->a, in->b, in->c};
        for (size_t arg = 0; arg < sizeof(inputs) / sizeof(*inputs); ++arg)
          valid = valid && inputs[arg] >= 0 && inputs[arg] < f->next_value &&
                  defined[inputs[arg]];
        if (!valid) {
          free(defined);
          return nir_verify_inst_err(f, err, err_len, in, i,
                                     "invalid repeated port IO operands");
        }
        break;
      }
      if (in->imm == -184 || in->imm == -185) {
        bool valid = in->dst >= 0 && in->symbol && in->b == -1 && in->c == -1 &&
                     in->d == -1 && in->e == -1 && in->f == -1 &&
                     !in->extra_args_len && !in->phi_incoming_len &&
                     (in->symbol[0] ? in->a == -1
                                    : in->a >= 0 && in->a < f->next_value &&
                                          defined[in->a]);
        if (!valid) {
          free(defined);
          return nir_verify_inst_err(f, err, err_len, in, i,
                                     "invalid segment guard load");
        }
        break;
      }
      if (in->imm == -182 || in->imm == -183) {
        bool symbol_valid =
            in->symbol &&
            (in->imm == -183 ? strcmp(in->symbol, "xsha256") == 0
                             : strcmp(in->symbol, "movsb") == 0 ||
                                   strcmp(in->symbol, "movsw") == 0 ||
                                   strcmp(in->symbol, "movsl") == 0 ||
                                   strcmp(in->symbol, "movsq") == 0);
        bool valid = symbol_valid && in->dst == -1 && in->e == -1 &&
                     in->f == -1 && !in->extra_args_len &&
                     !in->phi_incoming_len;
        const int inputs[] = {in->a, in->b, in->c, in->d};
        for (size_t arg = 0; arg < sizeof(inputs) / sizeof(*inputs); ++arg)
          valid = valid && inputs[arg] >= 0 && inputs[arg] < f->next_value &&
                  defined[inputs[arg]];
        if (!valid) {
          free(defined);
          return nir_verify_inst_err(f, err, err_len, in, i,
                                     "invalid fixed-register memory operands");
        }
        break;
      }
      if (cmpxchg16b_asm) {
        bool valid = in->dst >= 0 && in->a >= 0 && in->a < f->next_value &&
                     defined[in->a] && in->b >= 0 && in->b < f->next_value &&
                     defined[in->b] && in->c >= 0 && in->c < f->next_value &&
                     defined[in->c] && in->d == -1 && in->e == -1 &&
                     in->f == -1 && in->extra_args_len == 0 &&
                     !in->phi_incoming_len && in->symbol &&
                     (strcmp(in->symbol, "cmpxchg16b_try") == 0 ||
                      strcmp(in->symbol, "cmpxchg16b") == 0);
        if (!valid) {
          free(defined);
          return nir_verify_inst_err(f, err, err_len, in, i,
                                     "invalid cmpxchg16b operands");
        }
        break;
      }
      if (futex_cmpxchg_asm) {
        const int inputs[] = {in->a, in->b, in->c, in->d, in->e};
        bool valid = in->dst >= 0 && in->dst < f->next_value && in->symbol &&
                     strcmp(in->symbol, "futex_cmpxchg32_extable") == 0 &&
                     in->f == -1 && in->extra_args_len == 0 &&
                     !in->phi_incoming_len;
        for (size_t input = 0; input < sizeof(inputs) / sizeof(inputs[0]);
             ++input)
          valid = valid && inputs[input] >= 0 &&
                  inputs[input] < f->next_value && defined[inputs[input]];
        if (!valid) {
          free(defined);
          return nir_verify_inst_err(f, err, err_len, in, i,
                                     "invalid futex cmpxchg operands");
        }
        break;
      }
      if (futex_runtime_mask_asm) {
        bool valid = in->dst >= 0 && in->dst < f->next_value && in->a >= 0 &&
                     in->a < f->next_value && defined[in->a] && in->b == -1 &&
                     in->c == -1 && in->d == -1 && in->e == -1 && in->f == -1 &&
                     in->symbol &&
                     strcmp(in->symbol, "runtime_mask___futex_mask") == 0 &&
                     in->extra_args_len == 0 && !in->phi_incoming_len;
        if (!valid) {
          free(defined);
          return nir_verify_inst_err(f, err, err_len, in, i,
                                     "invalid futex runtime mask operands");
        }
        break;
      }
      if (futex_xchg_asm || futex_xadd_asm) {
        const int inputs[] = {in->a, in->b, in->c, in->d};
        bool valid =
            in->dst >= 0 && in->dst < f->next_value && in->symbol &&
            strcmp(in->symbol, futex_xchg_asm ? "futex_xchg32_extable"
                                              : "futex_xadd32_extable") == 0 &&
            in->e == -1 && in->f == -1 && in->extra_args_len == 0 &&
            !in->phi_incoming_len;
        for (size_t input = 0; input < sizeof(inputs) / sizeof(inputs[0]);
             ++input)
          valid = valid && inputs[input] >= 0 &&
                  inputs[input] < f->next_value && defined[inputs[input]];
        if (!valid) {
          free(defined);
          return nir_verify_inst_err(f, err, err_len, in, i,
                                     futex_xchg_asm
                                         ? "invalid futex xchg operands"
                                         : "invalid futex xadd operands");
        }
        break;
      }
      if (futex_atomic_op2_asm) {
        const int inputs[] = {in->a, in->b, in->c, in->d};
        bool valid =
            in->dst >= 0 && in->dst < f->next_value && in->symbol &&
            (strcmp(in->symbol, "or") == 0 || strcmp(in->symbol, "and") == 0 ||
             strcmp(in->symbol, "xor") == 0) &&
            in->e == -1 && in->f == -1 && in->extra_args_len == 0 &&
            !in->phi_incoming_len;
        for (size_t input = 0; input < sizeof(inputs) / sizeof(inputs[0]);
             ++input)
          valid = valid && inputs[input] >= 0 &&
                  inputs[input] < f->next_value && defined[inputs[input]];
        if (!valid) {
          free(defined);
          return nir_verify_inst_err(f, err, err_len, in, i,
                                     "invalid futex atomic operation operands");
        }
        break;
      }
      if (crc32_asm) {
        const int operands[] = {in->a, in->b};
        bool valid = in->dst >= 0 && in->dst < f->next_value && in->c == -1 &&
                     in->d == -1 && in->e == -1 && in->f == -1 && in->symbol &&
                     (strcmp(in->symbol, "crc32q") == 0 ||
                      strcmp(in->symbol, "crc32l") == 0 ||
                      strcmp(in->symbol, "crc32w") == 0 ||
                      strcmp(in->symbol, "crc32b") == 0) &&
                     in->extra_args_len == 0 && !in->phi_incoming_len;
        for (size_t operand = 0;
             operand < sizeof(operands) / sizeof(operands[0]); ++operand)
          valid = valid && operands[operand] >= 0 &&
                  operands[operand] < f->next_value &&
                  defined[operands[operand]];
        if (!valid) {
          free(defined);
          return nir_verify_inst_err(f, err, err_len, in, i,
                                     "invalid CRC32 operands");
        }
        break;
      }
      if (repeat_movsl_tail_asm) {
        const int operands[] = {in->a, in->b, in->c, in->d, in->e, in->f};
        bool valid = in->dst == -1 && in->symbol &&
                     strcmp(in->symbol, "rep_movsl_tail") == 0 &&
                     in->extra_args_len == 0 && !in->phi_incoming_len;
        for (size_t operand = 0;
             operand < sizeof(operands) / sizeof(operands[0]); ++operand)
          valid = valid && operands[operand] >= 0 &&
                  operands[operand] < f->next_value &&
                  defined[operands[operand]];
        if (!valid) {
          free(defined);
          return nir_verify_inst_err(f, err, err_len, in, i,
                                     "invalid rep-movsl-tail operands");
        }
        break;
      }
      if (xstore_asm) {
        const int operands[] = {in->a, in->b, in->c, in->d, in->e};
        bool valid = in->dst == -1 && in->f == -1 && in->symbol &&
                     strcmp(in->symbol, "xstore") == 0 &&
                     in->extra_args_len == 0 && !in->phi_incoming_len;
        for (size_t operand = 0;
             operand < sizeof(operands) / sizeof(operands[0]); ++operand)
          valid = valid && operands[operand] >= 0 &&
                  operands[operand] < f->next_value &&
                  defined[operands[operand]];
        if (!valid) {
          free(defined);
          return nir_verify_inst_err(f, err, err_len, in, i,
                                     "invalid xstore operands");
        }
        break;
      }
      if (movntdqa_asm) {
        bool valid_symbol =
            in->symbol && (strcmp(in->symbol, "movntdqa_movaps_4") == 0 ||
                           strcmp(in->symbol, "movntdqa_movaps_1") == 0 ||
                           strcmp(in->symbol, "movntdqa_movups_1") == 0 ||
                           strcmp(in->symbol, "movntdqa_movups_4") == 0);
        bool valid = in->dst == -1 && in->a >= 0 && in->b >= 0 && in->c == -1 &&
                     in->d == -1 && in->e == -1 && in->f == -1 &&
                     valid_symbol && in->extra_args_len == 0 &&
                     !in->phi_incoming_len && in->a < f->next_value &&
                     in->b < f->next_value && defined[in->a] && defined[in->b];
        if (!valid) {
          free(defined);
          return nir_verify_inst_err(f, err, err_len, in, i,
                                     "invalid movntdqa operands");
        }
        break;
      }
      if (((in->dst >= 0) != (builtin_result || register_output ||
                              condition_output || memory_register_output ||
                              faultable_load_goto || faultable_load_reg)) ||
          (debug_register_write &&
           (!in->symbol || strncmp(in->symbol, "db", 2) != 0 ||
            in->symbol[2] < '0' || in->symbol[2] > '7' ||
            in->symbol[3] != '\0')) ||
          (checked_segment_load_asm &&
           (in->dst != -1 || in->a < 0 || in->b != -1 || in->c != -1 ||
            in->d != -1 || in->e != -1 || in->f != -1 || !in->symbol ||
            (strcmp(in->symbol, "ss") != 0 && strcmp(in->symbol, "ds") != 0 &&
             strcmp(in->symbol, "es") != 0 &&
             strcmp(in->symbol, "fs") != 0))) ||
          (repeat_movsb_asm &&
           (in->dst != -1 || in->a < 0 || in->b < 0 || in->c < 0 || in->d < 0 ||
            in->e < 0 || in->f < 0 || !in->symbol ||
            strcmp(in->symbol, "rep_movsb") != 0)) ||
          (repeat_movsl_tail_asm &&
           (in->dst != -1 || in->a < 0 || in->b < 0 || in->c < 0 || in->d < 0 ||
            in->e < 0 || in->f < 0 || !in->symbol ||
            strcmp(in->symbol, "rep_movsl_tail") != 0)) ||
          (repeat_stosb_asm &&
           (in->dst != -1 || in->a < 0 || in->b < 0 || in->c < 0 || in->d < 0 ||
            in->e < 0 || in->f != -1 || !in->symbol ||
            (strcmp(in->symbol, "rep_stosb") != 0 &&
             strcmp(in->symbol, "rep_stosw") != 0 &&
             strcmp(in->symbol, "rep_stosl") != 0 &&
             strcmp(in->symbol, "rep_stosq") != 0))) ||
          (delay_loop_asm &&
           (in->dst < 0 || in->a < 0 || in->b != -1 || in->c != -1 ||
            in->d != -1 || in->e != -1 || in->f != -1 || !in->symbol ||
            strcmp(in->symbol, "x86_delay_loop") != 0)) ||
          (usercopy_movnti_asm &&
           (in->dst != -1 || in->a < 0 || in->b < 0 || in->c != -1 ||
            in->d != -1 || in->e != -1 || in->f != -1 || !in->symbol ||
            (strcmp(in->symbol, "usercopy_movnti_4x8") != 0 &&
             strcmp(in->symbol, "usercopy_movnti_1x8") != 0 &&
             strcmp(in->symbol, "usercopy_movnti_1x4") != 0))) ||
          (alt_reloc_selftest_asm &&
           (in->dst != -1 || in->a != -1 || in->b != -1 || in->c != -1 ||
            in->d != -1 || in->e != -1 || in->f != -1 || !in->symbol ||
            !strstr(in->symbol, "# ALT: oldinstr") ||
            !strstr(in->symbol, ".altinstructions") ||
            !strstr(in->symbol, "call __alt_reloc_selftest"))) ||
          (repeat_movs_alternative_asm &&
           (in->dst != -1 || in->a < 0 || in->b < 0 || in->c < 0 || in->d < 0 ||
            in->e < 0 || in->f < 0 || !in->symbol ||
            !strstr(in->symbol, "# ALT: oldinstr") ||
            !strstr(in->symbol, ".altinstructions") ||
            !strstr(in->symbol, "call rep_movs_alternative"))) ||
          (invlpg_asm &&
           (in->dst != -1 || in->a < 0 || in->b != -1 || in->c != -1 ||
            in->d != -1 || in->e != -1 || in->f != -1 || !in->symbol ||
            strcmp(in->symbol, "invlpg") != 0)) ||
          (lkgs_asm && (in->dst != -1 || in->b != -1 || in->c != -1 ||
                        !in->symbol || strcmp(in->symbol, "lkgs") != 0)) ||
          (clflush_asm &&
           (in->dst != -1 || in->a < 0 || in->b != -1 || in->c != -1 ||
            in->d != -1 || in->e != -1 || in->f != -1 || !in->symbol ||
            strcmp(in->symbol, "clflush") != 0)) ||
          (monitor_asm &&
           (in->dst != -1 || in->a < 0 || in->b < 0 || in->c < 0 ||
            in->b >= f->next_value || !defined[in->b] ||
            in->c >= f->next_value || !defined[in->c] || in->d != -1 ||
            in->e != -1 || in->f != -1 || !in->symbol ||
            (strcmp(in->symbol, "monitor") != 0 &&
             strcmp(in->symbol, "monitorx") != 0 &&
             strcmp(in->symbol, "mwaitx") != 0))) ||
          (fxsave_asm &&
           (in->dst != -1 || in->a < 0 || in->b != -1 || in->c != -1 ||
            in->d != -1 || in->e != -1 || in->f != -1 || !in->symbol ||
            (strcmp(in->symbol, "fxsave64") != 0 &&
             strcmp(in->symbol, "fxsave") != 0 &&
             strcmp(in->symbol, "fnsave") != 0))) ||
          (fninit_status_asm &&
           (in->dst != -1 || in->a < 0 || in->b < 0 || in->b >= f->next_value ||
            !defined[in->b] || in->c != -1 || in->d != -1 || in->e != -1 ||
            in->f != -1 || !in->symbol ||
            strcmp(in->symbol, "fninit_status") != 0)) ||
          (fdiv_bug_asm &&
           (in->dst != -1 || in->a < 0 || in->b < 0 || in->c < 0 ||
            in->b >= f->next_value || !defined[in->b] ||
            in->c >= f->next_value || !defined[in->c] || in->d != -1 ||
            in->e != -1 || in->f != -1 || !in->symbol ||
            strcmp(in->symbol, "fdiv_bug_probe") != 0)) ||
          (fildl_clear_asm &&
           (in->dst != -1 || in->a < 0 || in->b != -1 || in->c != -1 ||
            in->d != -1 || in->e != -1 || in->f != -1 || !in->symbol ||
            strcmp(in->symbol, "fildl_clear") != 0)) ||
          (ldmxcsr_asm &&
           (in->dst != -1 || in->a < 0 || in->b != -1 || in->c != -1 ||
            in->d != -1 || in->e != -1 || in->f != -1 || !in->symbol ||
            strcmp(in->symbol, "ldmxcsr") != 0)) ||
          (fxrstor_extable_asm &&
           (in->dst != -1 || in->a < 0 || in->b != -1 || in->c != -1 ||
            in->d != -1 || in->e != -1 || in->f != -1 || !in->symbol ||
            (strcmp(in->symbol, "fxrstor_extable") != 0 &&
             strcmp(in->symbol, "frstor_extable") != 0))) ||
          (fxrstor_error_asm &&
           (in->dst < 0 || in->a < 0 || in->b != -1 || in->c != -1 ||
            in->d != -1 || in->e != -1 || in->f != -1 || !in->symbol ||
            (strcmp(in->symbol, "fxrstor_error") != 0 &&
             strcmp(in->symbol, "frstor_error") != 0))) ||
          (fxrstor_user_asm &&
           (in->dst < 0 || in->a < 0 || in->b != -1 || in->c != -1 ||
            in->d != -1 || in->e != -1 || in->f != -1 || !in->symbol ||
            (strcmp(in->symbol, "fxrstor_user") != 0 &&
             strcmp(in->symbol, "frstor_user") != 0))) ||
          (xgetbv_asm &&
           (in->dst != -1 || in->a < 0 || in->b < 0 || in->c < 0 ||
            in->b >= f->next_value || !defined[in->b] ||
            in->c >= f->next_value || !defined[in->c] || in->d != -1 ||
            in->e != -1 || in->f != -1 || !in->symbol ||
            strcmp(in->symbol, "xgetbv") != 0)) ||
          (xsave_alternative_asm &&
           (in->dst != -1 || in->a < 0 || in->b < 0 || in->c < 0 || in->d < 0 ||
            in->b >= f->next_value || !defined[in->b] ||
            in->c >= f->next_value || !defined[in->c] ||
            in->d >= f->next_value || !defined[in->d] || in->e != -1 ||
            in->f != -1 || !in->symbol ||
            strcmp(in->symbol, "xsave_alternative") != 0)) ||
          (xrstor_alternative_asm &&
           (in->dst != -1 || in->a < 0 || in->b < 0 || in->c < 0 ||
            in->b >= f->next_value || !defined[in->b] ||
            in->c >= f->next_value || !defined[in->c] || in->d != -1 ||
            in->e != -1 || in->f != -1 || !in->symbol ||
            strcmp(in->symbol, "xrstor_alternative") != 0)) ||
          (fnsave_user_asm &&
           (in->dst < 0 || in->a < 0 || in->b != -1 || in->c != -1 ||
            in->d != -1 || in->e != -1 || in->f != -1 || !in->symbol ||
            (strcmp(in->symbol, "fnsave_user") != 0 &&
             strcmp(in->symbol, "fxsave_user") != 0))) ||
          (put_user_nocheck_asm &&
           (in->dst < 0 || in->a < 0 || in->b < 0 || in->c < 0 || in->d < 0 ||
            in->b >= f->next_value || !defined[in->b] ||
            in->c >= f->next_value || !defined[in->c] ||
            in->d >= f->next_value || !defined[in->d] || in->e != -1 ||
            in->f != -1 || !in->symbol ||
            strcmp(in->symbol, "put_user_nocheck") != 0)) ||
          (xsave_user_asm &&
           (in->dst < 0 || in->a < 0 || in->b < 0 || in->c < 0 || in->d < 0 ||
            in->b >= f->next_value || !defined[in->b] ||
            in->c >= f->next_value || !defined[in->c] ||
            in->d >= f->next_value || !defined[in->d] || in->e != -1 ||
            in->f != -1 || !in->symbol ||
            (strcmp(in->symbol, "xsave_user") != 0 &&
             strcmp(in->symbol, "xsaves_user") != 0 &&
             strcmp(in->symbol, "xrstor_user") != 0 &&
             strcmp(in->symbol, "xrstors_user") != 0))) ||
          (random_word_asm &&
           (in->dst < 0 || in->a < 0 || in->b != -1 || in->c != -1 ||
            in->d != -1 || in->e != -1 || in->f != -1 || !in->symbol ||
            (strcmp(in->symbol, "rdrand") != 0 &&
             strcmp(in->symbol, "rdseed") != 0))) ||
          (data_segment_reload_asm &&
           (in->dst != -1 || in->a < 0 || in->b != -1 || in->c != -1 ||
            in->d != -1 || in->e != -1 || in->f != -1 || !in->symbol ||
            strcmp(in->symbol, "reload_ds_ss_es") != 0)) ||
          (wrfsbase_asm &&
           (in->dst != -1 || in->a < 0 || in->b != -1 || in->c != -1 ||
            in->d != -1 || in->e != -1 || in->f != -1 || !in->symbol ||
            (strcmp(in->symbol, "wrfsbase") != 0 &&
             strcmp(in->symbol, "wrgsbase") != 0))) ||
          (atomic_arithmetic_condition &&
           (in->dst < 0 || in->a < 0 || in->b < 0 || in->c != -1 ||
            !in->symbol ||
            (strcmp(in->symbol, "addl") != 0 &&
             strcmp(in->symbol, "subl") != 0 &&
             strcmp(in->symbol, "addq") != 0 &&
             strcmp(in->symbol, "subq") != 0 &&
             strcmp(in->symbol, "addl_zero") != 0 &&
             strcmp(in->symbol, "subl_zero") != 0 &&
             strcmp(in->symbol, "addq_zero") != 0 &&
             strcmp(in->symbol, "subq_zero") != 0 &&
             strcmp(in->symbol, "addl_unlocked") != 0 &&
             strcmp(in->symbol, "subl_unlocked") != 0 &&
             strcmp(in->symbol, "addq_unlocked") != 0 &&
             strcmp(in->symbol, "subq_unlocked") != 0 &&
             strcmp(in->symbol, "addl_unlocked_zero") != 0 &&
             strcmp(in->symbol, "subl_unlocked_zero") != 0 &&
             strcmp(in->symbol, "addq_unlocked_zero") != 0 &&
             strcmp(in->symbol, "subq_unlocked_zero") != 0))) ||
          (faultable_store_goto &&
           (in->dst != -1 || !in->symbol ||
            (strcmp(in->symbol, "b") != 0 && strcmp(in->symbol, "w") != 0 &&
             strcmp(in->symbol, "l") != 0 && strcmp(in->symbol, "q") != 0))) ||
          (faultable_load_goto &&
           (in->dst < 0 || in->a < 0 || in->b != -1 || in->d != -1 ||
            in->e != -1 || in->f != -1 || !in->symbol ||
            (strcmp(in->symbol, "b") != 0 && strcmp(in->symbol, "w") != 0 &&
             strcmp(in->symbol, "l") != 0 && strcmp(in->symbol, "q") != 0))) ||
          (faultable_load_reg &&
           (in->dst < 0 || in->a < 0 || in->b < 0 || in->c < 0 || in->d != -1 ||
            in->e != -1 || in->f != -1 || !in->symbol ||
            (strncmp(in->symbol, "b;nyc_slot=", 11) != 0 &&
             strncmp(in->symbol, "w;nyc_slot=", 11) != 0 &&
             strncmp(in->symbol, "l;nyc_slot=", 11) != 0 &&
             strncmp(in->symbol, "q;nyc_slot=", 11) != 0))) ||
          in->a < -1 || in->a >= f->next_value ||
          (in->a >= 0 && !defined[in->a]) ||
          (get_user_asm || rdmsr_asm || rdpmc_asm || lkgs_asm ||
                   data_segment_reload_asm || typed_segment_load_asm
               ? in->b != -1
           : put_user_asm
               ? (in->b < 0 || in->b >= f->next_value || !defined[in->b])
           : rdmsr_safe_asm || rdmsr_safe_i386_asm
               ? (in->b < 0 || in->b >= f->next_value || !defined[in->b])
           : wrmsr_safe_asm
               ? (in->b < 0 || in->b >= f->next_value || !defined[in->b])
           : alternative_wrmsr_asm
               ? (in->b < 0 || in->b >= f->next_value || !defined[in->b])
           : bsr_asm ? (in->b < 0 || in->b >= f->next_value || !defined[in->b])
           : repeat_movsb_asm      ? (in->b >= f->next_value || !defined[in->b])
           : repeat_movsl_tail_asm ? (in->b >= f->next_value || !defined[in->b])
           : repeat_stosb_asm      ? (in->b >= f->next_value || !defined[in->b])
           : usercopy_movnti_asm
               ? (in->b < 0 || in->b >= f->next_value || !defined[in->b])
           : repeat_movs_alternative_asm
               ? (in->b >= f->next_value || !defined[in->b])
           : faultable_load_goto ? in->b != -1
           : faultable_load_reg
               ? (in->b < 0 || in->b >= f->next_value || !defined[in->b] ||
                  in->c >= f->next_value || !defined[in->c])
           : asm_goto ? (in->b < 0 || in->b >= f->next_value || !defined[in->b])
           : port_output_asm ? in->b != -1
           : random_word_asm ? in->b != -1
           : put_user_nocheck_asm || xsave_user_asm || syscall_asm ||
                   cpuid_asm || atomic_bitwise || atomic_bitwise64 ||
                   atomic_bitwise_condition || atomic_arithmetic_condition
               ? (in->b < 0 || in->b >= f->next_value || !defined[in->b])
           : syscall_i386_asm ? (in->b < -1 || in->b >= f->next_value ||
                                 (in->b >= 0 && !defined[in->b]))
           : divq_u64_asm
               ? (in->b < 0 || in->b >= f->next_value || !defined[in->b])
           : monitor_asm
               ? (in->b < 0 || in->b >= f->next_value || !defined[in->b])
           : fxrstor_error_asm || fxrstor_user_asm ? in->b != -1
           : xgetbv_asm || xsave_alternative_asm || xrstor_alternative_asm
               ? (in->b < 0 || in->b >= f->next_value || !defined[in->b])
           : fninit_status_asm
               ? (in->b < 0 || in->b >= f->next_value || !defined[in->b])
           : fdiv_bug_asm
               ? (in->b < 0 || in->b >= f->next_value || !defined[in->b])
           : irq_stack_switch_asm
               ? (in->b < 0 || in->b >= f->next_value || !defined[in->b])
           : call_on_stack_asm
               ? (in->b < -1 ||
                  (in->b >= 0 && (in->b >= f->next_value || !defined[in->b])))
           : indirect_jump_asm
               ? (in->b < 0 || in->b >= f->next_value || !defined[in->b])
           : gs_percpu_add_asm
               ? (in->b < 0 || in->b >= f->next_value || !defined[in->b])
           : unsigned_arithmetic_asm
               ? (in->b < 0 || in->b >= f->next_value || !defined[in->b])
           : in->imm == -3 || in->imm == -10 || in->imm == -11 ||
                   in->imm == -12 || in->imm == -155 || in->imm == -145 ||
                   in->imm == -146 || in->imm == -147 || in->imm == -16 ||
                   read_write_xchg || read_write_xadd
               ? (in->b < 0 || in->b >= f->next_value || !defined[in->b])
           : cmpxchg16b_asm
               ? (in->b < 0 || in->b >= f->next_value || !defined[in->b])
           : (fixed_register_input || memory_register_input)
               ? (in->b < -1 || in->b >= f->next_value ||
                  (in->b >= 0 && !defined[in->b]))
               : in->b != -1) ||
          ((cmpxchg16b_asm || fixed_register_input || port_output_asm ||
            divq_u64_asm || in->imm == -11 || in->imm == -12 ||
            in->imm == -155 || in->imm == -145 || in->imm == -16 ||
            syscall_asm || syscall_i386_asm || repeat_movsb_asm ||
            repeat_movsl_tail_asm || repeat_stosb_asm ||
            repeat_movs_alternative_asm || unsigned_arithmetic_asm ||
            wrmsr_safe_asm || alternative_wrmsr_asm) &&
           (in->c < -1 || in->c >= f->next_value ||
            (in->c >= 0 && !defined[in->c]))) ||
          (cmpxchg16b_asm &&
           (in->dst < 0 || in->a < 0 || in->b < 0 || in->c < 0 ||
            in->c >= f->next_value || !defined[in->c] || in->d != -1 ||
            in->e != -1 || in->f != -1 || !in->symbol ||
            strcmp(in->symbol, "cmpxchg16b_try") != 0)) ||
          (repeat_movsb_asm && (in->c >= f->next_value || !defined[in->c])) ||
          (repeat_movsl_tail_asm &&
           (in->c >= f->next_value || !defined[in->c])) ||
          (repeat_stosb_asm && (in->c >= f->next_value || !defined[in->c])) ||
          (repeat_movs_alternative_asm &&
           (in->c >= f->next_value || !defined[in->c])) ||
          (cpuid_asm &&
           (in->c < 0 || in->c >= f->next_value || !defined[in->c] ||
            !in->symbol || strcmp(in->symbol, "cpuid") != 0)) ||
          ((get_user_asm || rdmsr_asm || rdmsr_safe_asm ||
            rdmsr_safe_i386_asm || rdpmc_asm || rdpmc_i386_asm) &&
           in->c != -1) ||
          (put_user_nocheck_asm &&
           (in->c < 0 || in->c >= f->next_value || !defined[in->c])) ||
          (xsave_user_asm &&
           (in->c < 0 || in->c >= f->next_value || !defined[in->c])) ||
          (wrmsr_safe_asm &&
           (in->c < 0 || in->c >= f->next_value || !defined[in->c])) ||
          (alternative_wrmsr_asm &&
           (in->c < 0 || in->c >= f->next_value || !defined[in->c])) ||
          (irq_stack_switch_asm &&
           (in->c < -1 ||
            (in->c >= 0 && (in->c >= f->next_value || !defined[in->c])))) ||
          (call_on_stack_asm &&
           (in->c < -1 ||
            (in->c >= 0 && (in->c >= f->next_value || !defined[in->c])))) ||
          (!get_user_asm && !put_user_asm && !fixed_register_input &&
           !port_output_asm && !memory_load32_asm && !data_segment_reload_asm &&
           !iret_to_self_asm && !control_register_asm && !fs_segment_load_asm &&
           !typed_segment_load_asm && !descriptor_table_asm &&
           !task_register_load_asm && !verw_asm && !clflush_asm &&
           !monitor_asm && !fxsave_asm && !fninit_status_asm && !fdiv_bug_asm &&
           !fildl_clear_asm && !ldmxcsr_asm && !fxrstor_extable_asm &&
           !fxrstor_error_asm && !fxrstor_user_asm && !xgetbv_asm &&
           !xsave_alternative_asm && !xrstor_alternative_asm &&
           !fnsave_user_asm && !put_user_nocheck_asm && !xsave_user_asm &&
           !random_word_asm && !checked_segment_load_asm && !repeat_movsb_asm &&
           !repeat_stosb_asm && !repeat_movs_alternative_asm &&
           !irq_stack_switch_asm && !call_on_stack_asm && !asm_goto &&
           !faultable_load_reg && !syscall_asm && !syscall_i386_asm &&
           !cpuid_asm && !atomic_bitwise && !atomic_bitwise64 &&
           !atomic_bitwise_condition && !unsigned_arithmetic_asm &&
           !wrmsr_safe_asm && !alternative_wrmsr_asm && !divq_u64_asm &&
           !irq_stack_switch_asm && in->imm != -11 && in->imm != -12 &&
           in->imm != -155 && in->imm != -145 && in->imm != -16 &&
           in->c != -1) ||
          (control_register_write_asm &&
           (in->dst != -1 || in->a < 0 || in->b != -1 || in->c != -1)) ||
          (control_register_read_asm &&
           (in->dst < 0 || in->a != -1 || in->b != -1 || in->c != -1)) ||
          (memory_output && in->a < 0) || (memory_load32_asm && in->a < 0) ||
          (register_memory_input_output && in->a < 0) ||
          (memory_register_input && (in->a < 0 || in->b < 0)) ||
          (in->imm < 0 && !invlpg_asm && !far_jump_asm && !indirect_jump_asm &&
           !gs_percpu_add_asm && !kexec_segment_load_asm && !repeat_movsb_asm &&
           !repeat_movsl_tail_asm && !repeat_stosb_asm && !delay_loop_asm &&
           !usercopy_movnti_asm && !repeat_movs_alternative_asm &&
           !alt_reloc_selftest_asm && !get_user_asm && !put_user_asm &&
           !bsr_asm && !lkgs_asm && !typed_segment_load_asm && !cpuid_asm &&
           !rdmsr_asm && !rdmsr_safe_asm && !rdmsr_safe_i386_asm &&
           !rdpmc_i386_asm && !wrmsr_safe_asm && !rdpmc_asm &&
           !debug_register_write && !checked_segment_load_asm && !asm_goto &&
           !atomic_inc && !atomic_bitwise && !atomic_bitwise_condition &&
           !atomic_bitwise64 && !register_output && !memory_output &&
           !port_output_asm && !memory_load32_asm && !data_segment_reload_asm &&
           !iret_to_self_asm && !wrfsbase_asm && !control_register_asm &&
           !fs_segment_load_asm && !typed_segment_load_asm &&
           !descriptor_table_asm && !task_register_load_asm && !verw_asm &&
           !clflush_asm && !monitor_asm && !fxsave_asm && !fninit_status_asm &&
           !fdiv_bug_asm && !fildl_clear_asm && !ldmxcsr_asm &&
           !fxrstor_extable_asm && !fxrstor_error_asm && !fxrstor_user_asm &&
           !xgetbv_asm && !xsave_alternative_asm && !xrstor_alternative_asm &&
           !fnsave_user_asm && !put_user_nocheck_asm && !xsave_user_asm &&
           !random_word_asm && !fixed_register_input && !x86_rip_capture_asm &&
           !x86_rbp_capture_asm && !memory_register_input &&
           !alternative_wrmsr_asm && !divq_u64_asm && !wrfsbase_asm &&
           !irq_stack_switch_asm && !call_on_stack_asm && !faultable_load_reg &&
           !runtime_const_shift_asm && !cmpxchg16b_asm) ||
          (asm_goto && (in->c < 0 || !asm_goto_target)) ||
          (call_on_stack_asm &&
           (in->d < -1 ||
            (in->d >= 0 && (in->d >= f->next_value || !defined[in->d])))) ||
          (repeat_movsb_asm ? (in->d >= f->next_value || !defined[in->d] ||
                               in->e >= f->next_value || !defined[in->e] ||
                               in->f >= f->next_value || !defined[in->f])
           : repeat_movsl_tail_asm
               ? (in->d >= f->next_value || !defined[in->d] ||
                  in->e >= f->next_value || !defined[in->e] ||
                  in->f >= f->next_value || !defined[in->f])
           : repeat_stosb_asm ? (in->d >= f->next_value || !defined[in->d] ||
                                 in->e >= f->next_value || !defined[in->e])
           : repeat_movs_alternative_asm
               ? (in->d >= f->next_value || !defined[in->d] ||
                  in->e >= f->next_value || !defined[in->e] ||
                  in->f >= f->next_value || !defined[in->f])
           : cpuid_asm
               ? (in->d < 0 || in->d >= f->next_value || !defined[in->d] ||
                  in->e < 0 || in->e >= f->next_value || !defined[in->e] ||
                  in->f < 0 || in->f >= f->next_value || !defined[in->f])
           : get_user_asm
               ? (in->d < 0 || in->d >= f->next_value || !defined[in->d] ||
                  in->e < 0 || in->e >= f->next_value || !defined[in->e])
           : rdmsr_asm || rdmsr_safe_asm || rdmsr_safe_i386_asm || rdpmc_asm ||
                   rdpmc_i386_asm
               ? (in->d < 0 || in->d >= f->next_value || !defined[in->d] ||
                  (rdmsr_safe_i386_asm || rdpmc_i386_asm
                       ? in->e != -1
                       : in->e < 0 || in->e >= f->next_value ||
                             !defined[in->e]))
           : xsave_alternative_asm
               ? (in->d < 0 || in->d >= f->next_value || !defined[in->d])
           : xsave_user_asm
               ? (in->d < 0 || in->d >= f->next_value || !defined[in->d])
           : put_user_nocheck_asm
               ? (in->d < 0 || in->d >= f->next_value || !defined[in->d])
           : put_user_asm
               ? (in->d < 0 || in->d >= f->next_value || !defined[in->d])
           : wrmsr_safe_asm
               ? (in->d < 0 || in->d >= f->next_value || !defined[in->d])
           : alternative_wrmsr_asm
               ? (in->d < 0 || in->d >= f->next_value || !defined[in->d])
           : syscall_asm || syscall_i386_asm
               ? (in->d < -1 || in->d >= f->next_value ||
                  (in->d >= 0 && !defined[in->d]))
               : in->d != -1) ||
          (!get_user_asm && !put_user_asm && !repeat_movsb_asm &&
           !repeat_movsl_tail_asm && !repeat_stosb_asm &&
           !repeat_movs_alternative_asm && !rdmsr_asm && !rdmsr_safe_asm &&
           !rdmsr_safe_i386_asm && !rdpmc_i386_asm && !rdpmc_asm &&
           !cpuid_asm && !syscall_i386_asm && in->e != -1) ||
          (!cpuid_asm && !syscall_i386_asm && !repeat_movsb_asm &&
           !repeat_movsl_tail_asm && !repeat_stosb_asm &&
           !repeat_movs_alternative_asm && in->f != -1) ||
          (syscall_i386_asm &&
           (in->dst < 0 || in->a < 0 || !in->symbol ||
            strcmp(in->symbol, "syscall_i386") != 0 ||
            (in->b < 0 &&
             (in->c >= 0 || in->d >= 0 || in->e >= 0 || in->f >= 0)) ||
            (in->c < 0 && (in->d >= 0 || in->e >= 0 || in->f >= 0)) ||
            (in->d < 0 && (in->e >= 0 || in->f >= 0)) ||
            (in->e < 0 && in->f >= 0) ||
            (in->e >= 0 && (in->e >= f->next_value || !defined[in->e])) ||
            (in->f >= 0 && (in->f >= f->next_value || !defined[in->f])))) ||
          in->extra_args_len != 0 || !in->symbol ||
          (get_user_asm && !get_user_symbol_valid) ||
          (rdmsr_safe_i386_asm &&
           (in->dst != -1 || in->c != -1 || in->f != -1 ||
            strncmp(in->symbol, "rdmsr_safe_i386;nyc_slots=", 26) != 0)) ||
          (rdmsr_asm &&
           (in->dst != -1 || in->c != -1 || in->f != -1 ||
            (strncmp(in->symbol, "rdmsr;nyc_slots=", 16) != 0 &&
             strncmp(in->symbol, "rdmsr_mce;nyc_slots=", 20) != 0))) ||
          (rdpmc_i386_asm &&
           (in->dst != -1 || in->b != -1 || in->c != -1 || in->f != -1 ||
            strncmp(in->symbol, "rdpmc_i386;nyc_slot=", 20) != 0)) ||
          (divq_u64_asm &&
           (in->dst < 0 || in->a < 0 || in->b < 0 || in->c < 0 || in->d != -1 ||
            in->e != -1 || in->f != -1 ||
            strcmp(in->symbol, "divq_u64") != 0)) ||
          (rdpkru_asm && (in->dst < 0 || in->a < 0 || in->b != -1 ||
                          in->c != -1 || in->d != -1 || in->e != -1 ||
                          in->f != -1 || strcmp(in->symbol, "rdpkru") != 0)) ||
          (lar_valid_ss_asm &&
           (in->dst < 0 || in->a < 0 || in->b != -1 || in->c != -1 ||
            in->d != -1 || in->e != -1 || in->f != -1 ||
            strcmp(in->symbol, "lar-valid-ss") != 0)) ||
          (verw_asm &&
           (in->dst != -1 || in->a < 0 || in->b != -1 || in->c != -1 ||
            in->d != -1 || in->e != -1 || in->f != -1 ||
            (strcmp(in->symbol, "verw") != 0 &&
             strcmp(in->symbol, "verw_mem") != 0))) ||
          (irq_stack_switch_asm &&
           (in->dst != -1 || in->a < 0 || in->b < 0 || in->c < -1 ||
            in->d != -1 || in->e != -1 || in->f != -1)) ||
          (call_on_stack_asm &&
           (in->dst != -1 || in->a < 0 || in->b < -1 || in->c < -1 ||
            in->d < -1 || (in->c >= 0 && in->b < 0) ||
            (in->d >= 0 && in->c < 0) || in->e != -1 || in->f != -1 ||
            !in->symbol || !in->symbol[0])) ||
          (far_jump_asm &&
           (in->dst != -1 || in->a < 0 || in->c != -1 || in->d != -1 ||
            in->e != -1 || in->f != -1 || !in->symbol ||
            strcmp(in->symbol, "ljmpl *(%rax)") != 0)) ||
          (indirect_jump_asm &&
           (in->dst != -1 || in->a < 0 || in->b < 0 || in->c != -1 ||
            in->d != -1 || in->e != -1 || in->f != -1 || !in->symbol ||
            strcmp(in->symbol, "jmp *%0") != 0)) ||
          (gs_percpu_add_asm &&
           (in->dst != -1 || in->a < 0 || in->b < 0 || in->c != -1 ||
            in->d != -1 || in->e != -1 || in->f != -1 || !in->symbol ||
            (strcmp(in->symbol, "gs_addb") != 0 &&
             strcmp(in->symbol, "gs_addw") != 0 &&
             strcmp(in->symbol, "gs_addl") != 0 &&
             strcmp(in->symbol, "gs_addq") != 0))) ||
          (runtime_const_shift_asm &&
           (in->dst < 0 || in->a < 0 || in->b != -1 || in->c != -1 ||
            in->d != -1 || in->e != -1 || in->f != -1 ||
            !runtime_shift_symbol_valid)) ||
          (runtime_ptr_clamp_asm &&
           (in->dst < 0 || in->a < 0 || in->b < 0 || in->c != -1 ||
            in->d != -1 || in->e != -1 || in->f != -1 || in->symbol)) ||
          (apic_alt_io_asm &&
           (in->dst < 0 || in->a < 0 || in->b < 0 || in->c != -1 ||
            in->d != -1 || in->e != -1 || in->f != -1 || !in->symbol ||
            !strstr(in->symbol, "# ALT: oldinstr") ||
            !strstr(in->symbol, "movl %edx, (%rax)") ||
            !strstr(in->symbol, "xchgl %edx, (%rax)") ||
            !strstr(in->symbol, ".altinstructions"))) ||
          (kexec_segment_load_asm &&
           (in->dst != -1 || in->a < 0 || in->b != -1 || in->c != -1 ||
            in->d != -1 || in->e != -1 || in->f != -1 || !in->symbol ||
            !strstr(in->symbol, "movl %eax,%ds") ||
            !strstr(in->symbol, "movl %eax,%es") ||
            !strstr(in->symbol, "movl %eax,%ss") ||
            !strstr(in->symbol, "movl %eax,%fs") ||
            !strstr(in->symbol, "movl %eax,%gs")))) {

        free(defined);
        return nir_verify_inst_err(f, err, err_len, in, i,
                                   "invalid inline assembly operands");
      }
      break;
    }
    case NYIR_LABEL:
      if (nir_verify_labels ? nir_verify_first_label(in->imm) < i : false) {
        free(defined);
        return nir_verify_inst_err(f, err, err_len, in, i, "duplicate label");
      }
      break;
    case NYIR_CONST_I64:
    case NYIR_CONST_F64:
    case NYIR_CONST_F32:
      if (in->dst < 0) {
        free(defined);
        return nir_verify_inst_err(f, err, err_len, in, i,
                                   "constant has no destination");
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
    case NYIR_F64_TO_I64:
    case NYIR_F32_TO_I64:
    case NYIR_I64_TO_F32:
    case NYIR_F64_TO_F32:
    case NYIR_F32_TO_F64:
    case NYIR_BSWAP_I64:
    case NYIR_BSWAP32_I64:
    case NYIR_LOAD_I64:
    case NYIR_VEC4_LOAD_I64:
    case NYIR_VEC8_LOAD_I64:
    case NYIR_VEC4_LOAD_F64:
    case NYIR_VEC8_LOAD_F32:
    case NYIR_VEC4_SET1_F64:
    case NYIR_VEC8_SET1_F32:
    case NYIR_VEC4_SET1_I64:
    case NYIR_VEC8_SAR_I64:
    case NYIR_VEC8_SHR_I64:
      if (in->imm < 0 || in->imm > 63) {
        free(defined);
        return nir_verify_inst_err(
            f, err, err_len, in, i,
            "vector arithmetic shift count must be 0..63");
      }
      /*
       * fall through
       */
    case NYIR_VEC8_SET1_I64:
    case NYIR_VEC4_SHUFFLE_F64:
    case NYIR_VEC8_SHUFFLE_F32:
      if (in->dst < 0 || !nir_value_defined(f, defined, in->a)) {
        free(defined);
        return nir_verify_inst_err(f, err, err_len, in, i,
                                   "invalid unary value operand");
      }
      break;
    case NYIR_VA_ARG:
      if (in->dst < 0 || !nir_value_defined(f, defined, in->a) ||
          (in->imm != 0 && in->imm != 1) ||
          ((in->imm == 1) != (in->scalar_class == NYIR_SCALAR_F64))) {
        char detail[128];
        snprintf(detail, sizeof(detail),
                 "va.arg requires a defined va_list (v%d, defined=%s) and "
                 "matching scalar class",
                 in->a, nir_value_defined(f, defined, in->a) ? "yes" : "no");
        free(defined);
        return nir_verify_inst_err(f, err, err_len, in, i, detail);
      }
      break;
    case NYIR_SELECT_I64:
      if (in->dst < 0 || !nir_value_defined(f, defined, in->a) ||
          !nir_value_defined(f, defined, in->b) ||
          !nir_value_defined(f, defined, in->c) ||
          !nir_select_condition_is_boolean(f, in->a, i, 0)) {
        free(defined);
        return nir_verify_inst_err(
            f, err, err_len, in, i,
            "select.i64 requires an evaluated boolean and i64 values");
      }
      break;
    case NYIR_PHI:
      if (in->dst < 0 || in->phi_incoming_len == 0 || !in->phi_incoming) {
        free(defined);
        return nir_verify_inst_err(
            f, err, err_len, in, i,
            "phi requires a destination and incoming edges");
      }
      /*
       * PHIs may read a backedge value defined later in linear NYIR. CFG
       * validation below establishes the edge; require a real definition
       * somewhere rather than a linear predecessor here.
       */
      for (size_t k = 0; k < in->phi_incoming_len; ++k) {
        int v = in->phi_incoming[k].value;
        bool has_definition = v >= 0 && v < f->next_value &&
                              nir_verify_definitions &&
                              nir_verify_definitions[v];
        if (!nir_value_valid(f, v) || !has_definition) {
          free(defined);
          return nir_verify_inst_err(f, err, err_len, in, i,
                                     "phi has invalid incoming value");
        }
        for (size_t j = 0; j < k; ++j) {
          if (in->phi_incoming[j].predecessor_label ==
              in->phi_incoming[k].predecessor_label) {
            free(defined);
            return nir_verify_inst_err(f, err, err_len, in, i,
                                       "phi has duplicate predecessor");
          }
        }
      }
      break;
    case NYIR_LOAD_LOCAL:
      if (in->dst < 0 || in->imm < 0) {
        free(defined);
        return nir_verify_inst_err(f, err, err_len, in, i,
                                   "invalid local load");
      }
      break;
    case NYIR_STACK_GUARD_LOAD:
    case NYIR_STACK_GUARD_STORE: {
      bool store = in->op == NYIR_STACK_GUARD_STORE;
      bool valid = !in->imm && !in->symbol && !in->extra_args_len &&
                   !in->phi_incoming_len && in->b == -1 && in->c == -1 &&
                   in->d == -1 && in->e == -1 && in->f == -1 &&
                   (store ? in->dst == -1 && in->a >= 0 &&
                                in->a < f->next_value && defined[in->a]
                          : in->dst >= 0 && in->a == -1);
      if (!valid) {
        free(defined);
        return nir_verify_inst_err(f, err, err_len, in, i,
                                   "invalid native stack guard access");
      }
      break;
    }
    case NYIR_ADDR_LOCAL:
      if (in->dst < 0 || in->imm < 0) {
        free(defined);
        return nir_verify_inst_err(f, err, err_len, in, i,
                                   "invalid local address");
      }
      break;
    case NYIR_ADDR_SYMBOL:
      if (in->dst < 0 || !in->symbol || !in->symbol[0]) {
        free(defined);
        return nir_verify_inst_err(f, err, err_len, in, i,
                                   "addr.symbol requires a non-empty symbol");
      }
      break;
    case NYIR_READ_X86_RSP:
      if (in->dst < 0 || in->a != -1 || in->b != -1 || in->c != -1 ||
          in->d != -1 || in->e != -1 || in->f != -1 ||
          in->extra_args_len != 0) {
        free(defined);
        return nir_verify_inst_err(f, err, err_len, in, i,
                                   "read.x86.rsp requires a destination and "
                                   "no operands");
      }
      break;
    case NYIR_ALLOCA:
      if (in->dst < 0 || in->imm < 0) {
        free(defined);
        return nir_verify_inst_err(
            f, err, err_len, in, i,
            "alloca requires a valid destination and positive size");
      }
      break;
    case NYIR_VA_SAVE:
      if (in->dst < 0 || (in->imm != 48 && in->imm != 176)) {
        free(defined);
        return nir_verify_inst_err(f, err, err_len, in, i,
                                   "va.save requires a destination and 48- or "
                                   "176-byte SysV register area");
      }
      break;
    case NYIR_VA_START:
      if (in->dst >= 0 || !nir_value_defined(f, defined, in->a) ||
          !nir_value_defined(f, defined, in->b) || in->c != -1 || in->imm < 0 ||
          ((uint64_t)in->imm >> 32) > UINT32_MAX ||
          ((uint32_t)in->imm & 0xffffu) > 48 ||
          (((uint32_t)in->imm >> 16) < 48 || ((uint32_t)in->imm >> 16) > 176)) {
        free(defined);
        return nir_verify_inst_err(
            f, err, err_len, in, i,
            "va.start requires va_list, register area, and valid offsets");
      }
      break;
    case NYIR_COPY_STRUCT:
      if (in->a < 0 || in->b < 0 || in->imm < -2 ||
          (in->imm < 0
               ? (in->c < 0 || in->c >= f->next_value || !defined[in->c])
               : in->c != -1)) {
        free(defined);
        return nir_verify_inst_err(
            f, err, err_len, in, i,
            "copy.struct requires valid src, dst, and size");
      }
      break;
    case NYIR_CAPTURE_RET:
      if (in->dst < 0 || in->imm < 0 || in->imm > 13 || i == 0 ||
          (f->data[i - 1].op != NYIR_CALL &&
           f->data[i - 1].op != NYIR_CAPTURE_RET)) {
        free(defined);
        return nyr_inst_err(err, err_len, in, i,
                            "capture.ret requires a call/capture chain "
                            "immediately before it and selector 0..13");
      }
      break;
    case NYIR_STORE_LOCAL:
      if (in->imm < 0 || in->a < 0 || !nir_value_defined(f, defined, in->a)) {
        if (getenv("NYC_KERNEL_COMPAT")) {
          /*
           * Allow in kernel compat for trace macros
           */
          break;
        }
        free(defined);
        return nir_verify_inst_err(f, err, err_len, in, i,
                                   "invalid local store");
      }
      break;
    case NYIR_STORE_I64:
      if (!nir_value_defined(f, defined, in->a) ||
          !nir_value_defined(f, defined, in->c)) {
        free(defined);
        return nir_verify_inst_err(f, err, err_len, in, i,
                                   "invalid memory store");
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
        return nir_verify_inst_err(f, err, err_len, in, i,
                                   "invalid return value");
      }
      if (in->b >= 0 && !nir_value_defined(f, defined, in->b)) {
        free(defined);
        return nir_verify_inst_err(f, err, err_len, in, i,
                                   "invalid secondary return value");
      }
      if (in->b >= 0 &&
          !(f->return_aggregate_size &&
            f->return_aggregate_classes[0] != NYIR_ARG_CLASS_MEMORY &&
            f->return_aggregate_classes[1] != NYIR_ARG_CLASS_NONE)) {
        free(defined);
        return nir_verify_inst_err(f, err, err_len, in, i,
                                   "return carries a second piece without "
                                   "register-class aggregate return metadata");
      }
      break;
    case NYIR_BR:
      if (!nir_label_exists(f, in->imm)) {
        free(defined);
        return nir_verify_inst_err(f, err, err_len, in, i,
                                   "missing branch target label");
      }
      break;
    case NYIR_BR_IF:
      if (!nir_value_defined(f, defined, in->a) ||
          !nir_label_exists(f, in->imm)) {
        free(defined);
        return nir_verify_inst_err(
            f, err, err_len, in, i,
            "invalid conditional branch operand or target");
      }
      break;
    case NYIR_CALL:
      if (!(in->flags & NYIR_INST_F_INDIRECT) &&
          (!in->symbol || !in->symbol[0])) {
        free(defined);
        return nir_verify_inst_err(f, err, err_len, in, i,
                                   "call has no symbol");
      }
      if (in->imm < 0) {
        free(defined);
        return nir_verify_inst_err(f, err, err_len, in, i,
                                   "negative call arg count");
      }
      if (in->imm == 0 && (in->a >= 0 || in->b >= 0 || in->c >= 0 ||
                           in->d >= 0 || in->e >= 0 || in->f >= 0)) {
        free(defined);
        return nir_verify_inst_err(f, err, err_len, in, i,
                                   "zero-argument call has value operands");
      }
      if (in->imm == 1 && (in->b >= 0 || in->c >= 0 || in->d >= 0 ||
                           in->e >= 0 || in->f >= 0)) {
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
        return nir_verify_inst_err(
            f, err, err_len, in, i,
            "three-argument call has extra value operand");
      }
      if (in->imm == 4 && (in->e >= 0 || in->f >= 0)) {
        free(defined);
        return nir_verify_inst_err(
            f, err, err_len, in, i,
            "four-argument call has extra value operand");
      }
      if (in->imm == 5 && in->f >= 0) {
        free(defined);
        return nir_verify_inst_err(
            f, err, err_len, in, i,
            "five-argument call has extra value operand");
      }
      if (in->imm > 0 && !nir_value_defined(f, defined, in->a)) {
        free(defined);
        return nir_verify_inst_err(f, err, err_len, in, i,
                                   "call arg0 is invalid");
      }
      if (in->imm > 1 && !nir_value_defined(f, defined, in->b)) {
        free(defined);
        return nir_verify_inst_err(f, err, err_len, in, i,
                                   "call arg1 is invalid");
      }
      if (in->imm > 2 && !nir_value_defined(f, defined, in->c)) {
        free(defined);
        return nir_verify_inst_err(f, err, err_len, in, i,
                                   "call arg2 is invalid");
      }
      if (in->imm > 3 && !nir_value_defined(f, defined, in->d)) {
        free(defined);
        return nir_verify_inst_err(f, err, err_len, in, i,
                                   "call arg3 is invalid");
      }
      if (in->imm > 4 && !nir_value_defined(f, defined, in->e)) {
        free(defined);
        return nir_verify_inst_err(f, err, err_len, in, i,
                                   "call arg4 is invalid");
      }
      if (in->imm > 5 && !nir_value_defined(f, defined, in->f)) {
        free(defined);
        return nir_verify_inst_err(f, err, err_len, in, i,
                                   "call arg5 is invalid");
      }
      if (in->imm > NYIR_CALL_MAX_ARGS) {
        free(defined);
        return nir_verify_inst_err(
            f, err, err_len, in, i,
            "call exceeds the maximum supported argument count");
      }
      if (in->arg_sizes && in->imm <= 0) {
        free(defined);
        return nir_verify_inst_err(
            f, err, err_len, in, i,
            "zero-argument call has aggregate argument metadata");
      }
      if (in->arg_sizes) {
        for (int64_t arg = 0; arg < in->imm; ++arg) {
          uint32_t packed = in->arg_sizes[arg];
          unsigned c0 = NYIR_ARG_AGG_CLASS(packed, 0);
          unsigned c1 = NYIR_ARG_AGG_CLASS(packed, 1);
          if (packed != 0 &&
              !nir_aggregate_abi_valid(NYIR_ARG_AGG_SIZE(packed), c0, c1)) {
            free(defined);
            return nir_verify_inst_err(f, err, err_len, in, i,
                                       "invalid aggregate argument metadata");
          }
        }
      }
      if (in->ret_aggregate_size) {
        if (!nir_aggregate_abi_valid(in->ret_aggregate_size,
                                     in->ret_aggregate_classes[0],
                                     in->ret_aggregate_classes[1])) {
          free(defined);
          return nir_verify_inst_err(f, err, err_len, in, i,
                                     "invalid aggregate return metadata");
        }
      } else if (in->ret_aggregate_classes[0] != NYIR_ARG_CLASS_NONE ||
                 in->ret_aggregate_classes[1] != NYIR_ARG_CLASS_NONE) {
        free(defined);
        return nir_verify_inst_err(
            f, err, err_len, in, i,
            "aggregate return classes have no return size");
      }
      if (in->imm <= 6) {
        if (in->extra_args || in->extra_args_len != 0) {
          free(defined);
          return nir_verify_inst_err(
              f, err, err_len, in, i,
              "call has stray stack-args for a register-only arity");
        }
      } else {
        size_t want = (size_t)(in->imm - 6);
        if (!in->extra_args || in->extra_args_len != want) {
          free(defined);
          return nir_verify_inst_err(
              f, err, err_len, in, i,
              "call stack-arg count does not match arity");
        }
        for (size_t k = 0; k < want; ++k) {
          if (!nir_value_defined(f, defined, in->extra_args[k])) {
            free(defined);
            return nir_verify_inst_err(f, err, err_len, in, i,
                                       "call stack-arg is invalid");
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
        return nir_verify_inst_err(f, err, err_len, in, i,
                                   "invalid value operands");
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
  nir_verify_types_t shared_types = {0};
  bool scalar_ok =
      nir_verify_select_types(f, &shared_types, err, err_len) &&
      nir_verify_call_scalar_classes(f, &shared_types, err, err_len) &&
      nir_verify_store_scalar_classes(f, &shared_types, err, err_len) &&
      nir_verify_local_store_scalar_classes(f, &shared_types, err, err_len) &&
      nir_verify_return_scalar_classes(f, &shared_types, err, err_len);
  nyr_type_map_free(&shared_types.types);
  if (!scalar_ok)
    return false;
  if (!nir_verify_ownership_flow(f, err, err_len))
    return false;
  /*
   * COPY-chain and PHI-join ownership agreement (CALL args are covered by
   * nir_verify_ownership_flow above).
   */
  if (!nir_verify_ownership_consistency(f, err, err_len))
    return false;
  if (!nyr_validate_constraints(f, err, err_len))
    return false;
  if (err && err_len > 0)
    err[0] = '\0';
  return true;
}

bool nyr_analyze_binary_fold(nyr_op_t op, int64_t a, int64_t b, int64_t *out) {
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
  /*
   * Shifts and rotates mask their count exactly like the hardware (x86
   * masks variable counts to 6 bits, ROR32 to 5) and like the vector
   * evaluation path. Refusing out-of-range counts here made translation
   * validation report spurious "eval divergence" whenever a variable
   * shift count reached 64+, because only the VM side aborted.
   */
  case NYIR_SHL_I64:
    *out = (int64_t)((uint64_t)a << ((uint64_t)b & 63u));
    return true;
  case NYIR_SAR_I64:
    *out = a >> ((uint64_t)b & 63u);
    return true;
  case NYIR_ROR_I64: {
    unsigned shift = (unsigned)((uint64_t)b & 63u);
    if (shift == 0)
      *out = a;
    else
      *out = (int64_t)(((uint64_t)a >> shift) | ((uint64_t)a << (64 - shift)));
    return true;
  }
  case NYIR_ROR32_I64: {
    uint32_t word = (uint32_t)a;
    unsigned shift = (unsigned)((uint64_t)b & 31u);
    *out = (int64_t)(uint32_t)(shift == 0
                                   ? word
                                   : (word >> shift) | (word << (32 - shift)));
    return true;
  }
  default:
    return false;
  }
}

bool nyr_analyze_cmp_fold(nyr_cmp_t cmp, int64_t a, int64_t b, int64_t *out) {
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

static void nyr_fact_set_const(nyr_value_fact_t *fact, int64_t value) {
  if (!fact)
    return;
  fact->known_const = true;
  fact->const_value = value;
  fact->range.has_min = true;
  fact->range.has_max = true;
  fact->range.min = value;
  fact->range.max = value;
}

static bool nyr_i64_add_checked(int64_t a, int64_t b, int64_t *out) {
  if (!out)
    return false;
  if ((b > 0 && a > INT64_MAX - b) || (b < 0 && a < INT64_MIN - b))
    return false;
  *out = a + b;
  return true;
}

static bool nyr_i64_sub_checked(int64_t a, int64_t b, int64_t *out) {
  if (!out)
    return false;
  if ((b < 0 && a > INT64_MAX + b) || (b > 0 && a < INT64_MIN + b))
    return false;
  *out = a - b;
  return true;
}

static bool nyr_i64_div_checked(int64_t a, int64_t b, int64_t *out) {
  if (!out || b == 0 || (a == INT64_MIN && b == -1))
    return false;
  *out = a / b;
  return true;
}

static bool nyr_i64_mul_checked(int64_t a, int64_t b, int64_t *out) {
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

static bool nyr_range_from_corners(const int64_t *v, size_t n,
                                   nyr_range_t *out) {
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
  *out = (nyr_range_t){.has_min = true, .has_max = true, .min = lo, .max = hi};
  return true;
}

static bool nyr_fact_binary_range(nyr_op_t op, const nyr_value_fact_t *a,
                                  const nyr_value_fact_t *b, nyr_range_t *out) {
  if (!a || !b || !out)
    return false;
  if (op == NYIR_AND_I64) {
    if (b->known_const && b->const_value >= 0) {
      *out = (nyr_range_t){
          .has_min = true, .has_max = true, .min = 0, .max = b->const_value};
      return true;
    }
    if (a->known_const && a->const_value >= 0) {
      *out = (nyr_range_t){
          .has_min = true, .has_max = true, .min = 0, .max = a->const_value};
      return true;
    }
  }
  int64_t lo = 0;
  int64_t hi = 0;
  switch (op) {
  case NYIR_ADD_I64: {
    nyr_range_t r = {0};
    if (a->range.has_min && b->range.has_min &&
        nyr_i64_add_checked(a->range.min, b->range.min, &lo)) {
      r.has_min = true;
      r.min = lo;
    }
    if (a->range.has_max && b->range.has_max &&
        nyr_i64_add_checked(a->range.max, b->range.max, &hi)) {
      r.has_max = true;
      r.max = hi;
    }
    if (!r.has_min && !r.has_max)
      return false;
    *out = r;
    return true;
  }
  case NYIR_SUB_I64: {
    nyr_range_t r = {0};
    if (a->range.has_min && b->range.has_max &&
        nyr_i64_sub_checked(a->range.min, b->range.max, &lo)) {
      r.has_min = true;
      r.min = lo;
    }
    if (a->range.has_max && b->range.has_min &&
        nyr_i64_sub_checked(a->range.max, b->range.min, &hi)) {
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
    if (!nyr_i64_mul_checked(a->range.min, b->range.min, &corners[0]) ||
        !nyr_i64_mul_checked(a->range.min, b->range.max, &corners[1]) ||
        !nyr_i64_mul_checked(a->range.max, b->range.min, &corners[2]) ||
        !nyr_i64_mul_checked(a->range.max, b->range.max, &corners[3]) ||
        !nyr_range_from_corners(corners, 4, out))
      return false;
    return true;
  }
  case NYIR_DIV_I64: {
    int64_t d = 0;
    if (nyr_value_fact_singleton(b, &d) && d > 0) {
      nyr_range_t r = {0};
      if (a->range.has_min) {
        r.has_min = true;
        r.min = a->range.min / d;
      }
      if (a->range.has_max) {
        r.has_max = true;
        r.max = a->range.max / d;
      }
      if (r.has_min || r.has_max) {
        *out = r;
        return true;
      }
    }
    if (!a->range.has_min || !a->range.has_max || !b->range.has_min ||
        !b->range.has_max)
      return false;
    if (b->range.min <= 0 && b->range.max >= 0)
      return false;
    int64_t corners[4];
    if (!nyr_i64_div_checked(a->range.min, b->range.min, &corners[0]) ||
        !nyr_i64_div_checked(a->range.min, b->range.max, &corners[1]) ||
        !nyr_i64_div_checked(a->range.max, b->range.min, &corners[2]) ||
        !nyr_i64_div_checked(a->range.max, b->range.max, &corners[3]) ||
        !nyr_range_from_corners(corners, 4, out))
      return false;
    return true;
  }
  case NYIR_MOD_I64: {
    int64_t mod = 0;
    if (!nyr_value_fact_singleton(b, &mod) || mod == 0 || mod == INT64_MIN)
      return false;
    int64_t mod_abs = mod < 0 ? (mod == INT64_MIN ? INT64_MAX : -mod) : mod;
    int64_t limit = mod_abs - 1;
    nyr_range_t r = {0};
    r.has_min = true;
    r.has_max = true;
    if (a->range.has_min && a->range.min >= 0) {
      r.min = 0;
      r.max = limit;
      if (a->range.has_max && a->range.max < limit)
        r.max = a->range.max;
    } else if (a->range.has_max && a->range.max <= 0) {
      r.min = -limit;
      r.max = 0;
      if (a->range.has_min && a->range.min > -limit)
        r.min = a->range.min;
    } else {
      r.min = -limit;
      r.max = limit;
    }
    *out = r;
    return true;
  }
  case NYIR_SHL_I64: {
    if (!a->range.has_min || !a->range.has_max || !b->range.has_min ||
        !b->range.has_max)
      return false;
    int64_t shift = 0;
    if (!nyr_value_fact_singleton(b, &shift) || shift < 0 || shift >= 63 ||
        a->range.min < 0)
      return false;
    if (!nyr_i64_mul_checked(a->range.min, (int64_t)1 << shift, &lo) ||
        !nyr_i64_mul_checked(a->range.max, (int64_t)1 << shift, &hi))
      return false;
    break;
  }
  case NYIR_SAR_I64: {
    int64_t shift = 0;
    if (!nyr_value_fact_singleton(b, &shift) || shift < 0 || shift >= 64)
      return false;
    nyr_range_t r = {0};
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
  case NYIR_BSWAP_I64:
    return false;
  case NYIR_BSWAP32_I64:
  case NYIR_ROR32_I64:
    *out = (nyr_range_t){
        .has_min = true, .has_max = true, .min = 0, .max = 0xffffffffLL};
    return true;
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
  *out = (nyr_range_t){.has_min = true, .has_max = true, .min = lo, .max = hi};
  return true;
}

bool nyr_metadata_summary(const nyr_func_t *f, nyr_metadata_summary_t *summary,
                          char *err, size_t err_len) {
  if (!f)
    return nyr_err(err, err_len, "native NYIR metadata: missing function");
  if (!summary)
    return nyr_err(err, err_len,
                   "native NYIR metadata: missing summary output");
  char verify_err[256] = {0};
  if (!nyr_ir_verify(f, verify_err, sizeof(verify_err)))
    return nyr_err(err, err_len,
                   "native NYIR metadata: verifier rejected input: %s",
                   verify_err);
  memset(summary, 0, sizeof(*summary));
  summary->instructions = f->len;
  summary->values = f->next_value > 0 ? (size_t)f->next_value : 0;
  summary->vectorize_attempted_loops = f->vectorize_attempted_loops;
  summary->vectorize_rejected_loops = f->vectorize_rejected_loops;
  summary->vectorized_loops = f->vectorized_loops;
  for (size_t i = 0; i < f->len; ++i) {
    const nyr_inst_t *in = &f->data[i];
    if (in->op >= 0 && in->op < NYIR_OP_COUNT)
      summary->ops[in->op]++;
    summary->effect_mask |= in->effects | nyr_inst_effects(in);
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
  summary->locals = nyr_max_local(f);
  if (err && err_len > 0)
    err[0] = '\0';
  return true;
}

void nyr_metadata_summary_dump(FILE *out, const char *name,
                               const nyr_metadata_summary_t *summary) {
  if (!out || !summary)
    return;
  fprintf(
      out,
      "nyir metadata function=%s insts=%zu values=%zu locals=%zu labels=%zu "
      "branches=%zu br_if=%zu calls=%zu returns=%zu ranges=%zu debug=%zu "
      "vector_attempted=%zu vector_rejected=%zu vectorized=%zu effects=0x%x\n",
      name && name[0] ? name : "<anon>", summary->instructions, summary->values,
      summary->locals, summary->labels, summary->branches,
      summary->conditional_branches, summary->calls, summary->returns,
      summary->range_facts, summary->debug_locs,
      summary->vectorize_attempted_loops, summary->vectorize_rejected_loops,
      summary->vectorized_loops, summary->effect_mask);
  for (int op = 0; op < NYIR_OP_COUNT; ++op) {
    if (summary->ops[op])
      fprintf(out, "  op %-14s %zu\n", nyr_op_name((nyr_op_t)op),
              summary->ops[op]);
  }
}

bool nyr_analyze_values(const nyr_func_t *f, nyr_value_fact_t *facts,
                        size_t fact_count, char *err, size_t err_len) {
  if (!f)
    return nyr_err(err, err_len, "native NYIR analysis: missing function");
  if (f->next_value < 0)
    return nyr_err(err, err_len, "native NYIR analysis: invalid value count");
  if ((size_t)f->next_value > fact_count)
    return nyr_err(err, err_len,
                   "native NYIR analysis: fact table too small (%zu < %d)",
                   fact_count, f->next_value);
  if (facts && fact_count > 0)
    memset(facts, 0, fact_count * sizeof(*facts));

  for (size_t i = 0; i < f->len; ++i) {
    const nyr_inst_t *in = &f->data[i];
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
    nyr_value_fact_t *dst = &facts[in->dst];
    dst->effects |= in->effects | nyr_inst_effects(in);
    if (in->range.has_min || in->range.has_max)
      dst->range = in->range;

    switch (in->op) {
    case NYIR_CONST_I64:
      nyr_fact_set_const(dst, in->imm);
      break;
    case NYIR_COPY:
      if (in->a >= 0 && (size_t)in->a < fact_count) {
        dst->known_const = facts[in->a].known_const;
        dst->const_value = facts[in->a].const_value;
        dst->range = facts[in->a].range;
      }
      break;
    case NYIR_PHI: {
      if (in->phi_incoming_len > 0 && !in->phi_incoming)
        break;
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
        const nyr_value_fact_t *input = &facts[v];
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
        nyr_fact_set_const(dst, const_value);
      else if (have_input && (have_min || have_max))
        dst->range = (nyr_range_t){
            .has_min = have_min, .has_max = have_max, .min = lo, .max = hi};
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
        if (nyr_analyze_cmp_fold(in->cmp, facts[in->a].const_value,
                                 facts[in->b].const_value, &folded))
          nyr_fact_set_const(dst, folded);
      }
      break;
    default:
      if (in->a >= 0 && in->b >= 0 && (size_t)in->a < fact_count &&
          (size_t)in->b < fact_count && facts[in->a].known_const &&
          facts[in->b].known_const) {
        int64_t folded = 0;
        if (nyr_analyze_binary_fold(in->op, facts[in->a].const_value,
                                    facts[in->b].const_value, &folded)) {
          nyr_fact_set_const(dst, folded);
          break;
        }
      }
      if (in->a >= 0 && in->b >= 0 && (size_t)in->a < fact_count &&
          (size_t)in->b < fact_count) {
        nyr_range_t r = {0};
        if (nyr_fact_binary_range(in->op, &facts[in->a], &facts[in->b], &r)) {
          if (in->range.has_min && in->range.has_max && r.has_min &&
              r.has_max && (in->range.min > r.max || in->range.max < r.min)) {
            fprintf(stderr,
                    "DBG RANGE CONTRADICT op=%d inst=%zu dst=%d a=%d b=%d "
                    "in_range=[%lld,%lld] derived=[%lld,%lld]\n",
                    in->op, i, in->dst, in->a, in->b, (long long)in->range.min,
                    (long long)in->range.max, (long long)r.min,
                    (long long)r.max);
            free(facts);
            return nir_verify_inst_err(
                f, err, err_len, in, i,
                "instruction range fact contradicts derived range");
          }
          dst->range = r;
        }
      }
      break;
    }
  }
  if (err && err_len > 0)
    err[0] = '\0';
  return true;
}

bool nyr_validate_constraints(const nyr_func_t *f, char *err, size_t err_len) {
  if (!f)
    return nyr_err(err, err_len, "native NYIR constraints: missing function");
  bool *known = NULL;
  int64_t *value = NULL;
  if (f->next_value > 0) {
    known = (bool *)calloc((size_t)f->next_value, sizeof(bool));
    value = (int64_t *)calloc((size_t)f->next_value, sizeof(int64_t));
    if (!known || !value) {
      free(known);
      free(value);
      return nyr_err(err, err_len, "native NYIR constraints: out of memory");
    }
  }
  for (size_t i = 0; i < f->len; ++i) {
    const nyr_inst_t *in = &f->data[i];
    if (in->op < 0 || in->op >= NYIR_OP_COUNT) {
      free(known);
      free(value);
      return nir_verify_inst_err(f, err, err_len, in, i, "unknown opcode");
    }
    if (in->range.has_min && in->range.has_max &&
        in->range.min > in->range.max) {
      free(known);
      free(value);
      return nir_verify_inst_err(f, err, err_len, in, i,
                                 "range minimum exceeds maximum");
    }
    if ((in->op == NYIR_DIV_I64 || in->op == NYIR_MOD_I64) && in->b >= 0 &&
        known && known[in->b] && value[in->b] == 0) {
      free(known);
      free(value);
      return nir_verify_inst_err(f, err, err_len, in, i,
                                 "constant divide/modulo by zero");
    }
    if ((in->op == NYIR_DIV_I64 || in->op == NYIR_MOD_I64) && in->a >= 0 &&
        in->b >= 0 && known && known[in->a] && known[in->b] &&
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
