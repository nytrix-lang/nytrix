/*
 * NYIR verifier: structural and semantic sanity checks on the IR
 * after each transform pass to catch malformed ops or type mismatches.
 */
#include "base/common.h"
#include "base/compat.h"
#include "code/backend/ir/internal.h"
#include "code/backend/ir/opt/util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool nir_value_valid(const nyr_func_t *f, int v) {
  return v >= 0 && v < f->next_value;
}

static bool nir_aggregate_abi_valid(uint32_t size, unsigned c0, unsigned c1) {
  if (!size || size > NYIR_ARG_AGG_SIZE_MASK ||
      c0 > NYIR_ARG_CLASS_AAPCS_INTEGER_A16 ||
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

static bool nir_value_defined(const nyr_func_t *f, const bool *defined,
                              int v) {
  return nir_value_valid(f, v) && defined && defined[v];
}

static bool nir_label_exists(const nyr_func_t *f, int64_t label) {
  if (!f)
    return false;
  for (size_t i = 0; i < f->len; ++i) {
    const nyr_inst_t *in = &f->data[i];
    if (in->op == NYIR_LABEL && in->imm == label)
      return true;
  }
  return false;
}

bool nyr_label_referenced(const nyr_func_t *f, int64_t label) {
  if (!f)
    return false;
  for (size_t i = 0; i < f->len; ++i) {
    const nyr_inst_t *in = &f->data[i];
    if ((in->op == NYIR_BR || in->op == NYIR_BR_IF) && in->imm == label)
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
  case NYIR_CALL:
    allowed = NYIR_INST_F_EXTERN | NYIR_INST_F_SRET |
              NYIR_INST_F_EFFECTS_KNOWN | NYIR_INST_F_NOALIAS;
    break;
  case NYIR_RET:
    allowed = 0;
    break;
  case NYIR_LOAD_I64:
  case NYIR_STORE_I64:
    allowed = NYIR_INST_F_MEM_BYTE;
    break;
  case NYIR_ADD_I64:
    allowed = NYIR_INST_F_NARROW32 | NYIR_INST_F_NO_OVERFLOW |
              NYIR_INST_F_IN_BOUNDS_ADDR;
    break;
  case NYIR_SUB_I64:
  case NYIR_MUL_I64:
  case NYIR_AND_I64:
  case NYIR_OR_I64:
  case NYIR_XOR_I64:
  case NYIR_DIV_I64:
  case NYIR_MOD_I64:
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
  if (value < 0 || !definitions || definitions[value] < 0)
    return nir_verify_inst_err(f, err, err_len, in, use,
                               "value has no definition");
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
    return def->op == NYIR_COPY &&
           nir_select_condition_is_boolean(f, def->a, i, depth + 1);
  }
  return false;
}

static bool nir_verify_select_types(const nyr_func_t *f, char *err,
                                    size_t err_len) {
  nyr_type_map_t types = {0};
  if (!nyr_type_map_init(&types, f, nyr_max_local(f)))
    return nyr_err(err, err_len, "native NYIR verify: cannot build type map");
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
        nyr_type_map_free(&types);
        return nir_verify_inst_err(f, err, err_len, in, i,
                                   "select.i64 requires i64 operands");
      }
    }
  }
  nyr_type_map_free(&types);
  return true;
}

/*
 * Resolve only representation facts carried by NYIR itself. Copies preserve
 * their input class, and a PHI preserves a class only when every incoming
 * value is known and agrees. Do not infer raw/tagged or pointer classes from
 * integer-shaped opcodes.
 */
static nyr_scalar_class_t
nir_verify_value_scalar_class(const nyr_func_t *f,
                              const nyr_type_map_t *types, int value,
                              unsigned depth) {
  if (!f || value < 0 || value >= f->next_value || depth > 32)
    return NYIR_SCALAR_UNKNOWN;
  const nyr_inst_t *def = NULL;
  for (size_t i = f->len; i > 0; --i) {
    if (f->data[i - 1].dst == value) {
      def = &f->data[i - 1];
      break;
    }
  }
  if (!def)
    return NYIR_SCALAR_UNKNOWN;
  /*
   * A local load may retain the class of a former occupant of a reused
   * slot. Prefer a concrete same-block reaching store before trusting that
   * stale stamp. Control-flow and call boundaries make the reaching relation
   * ambiguous, so in those cases preserve the instruction's own class.
   */
  if (def->op == NYIR_LOAD_LOCAL) {
    size_t def_at = 0;
    for (; def_at < f->len && &f->data[def_at] != def; ++def_at) {
    }
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

static bool nir_verify_call_scalar_classes(const nyr_func_t *f, char *err,
                                           size_t err_len) {
  bool has_call = false;
  for (size_t i = 0; i < f->len; ++i)
    has_call |= f->data[i].op == NYIR_CALL;
  if (!has_call)
    return true;

  nyr_type_map_t types = {0};
  if (!nyr_type_map_init(&types, f, nyr_max_local(f)))
    return nyr_err(err, err_len,
                    "native NYIR verify: cannot build call scalar type map");
  for (size_t i = 0; i < f->len; ++i) {
    const nyr_inst_t *in = &f->data[i];
    if (in->op != NYIR_CALL)
      continue;
    int args[NYIR_CALL_MAX_ARGS];
    int argc = 0;
    char call_err[128] = {0};
    if (!nyr_call_args(in, f->next_value, args, NYIR_CALL_MAX_ARGS, &argc,
                        call_err, sizeof(call_err))) {
      nyr_type_map_free(&types);
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
        nyr_type_map_free(&types);
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
        nyr_type_map_free(&types);
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
          nyr_type_map_free(&types);
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
      nyr_type_map_free(&types);
      return nir_verify_inst_err(f, err, err_len, in, i, reason);
    }
  }
  nyr_type_map_free(&types);
  return true;
}

static bool nir_verify_store_scalar_classes(const nyr_func_t *f, char *err,
                                            size_t err_len) {
  bool has_store = false;
  for (size_t i = 0; i < f->len; ++i)
    has_store |= f->data[i].op == NYIR_STORE_I64;
  if (!has_store)
    return true;

  nyr_type_map_t types = {0};
  if (!nyr_type_map_init(&types, f, nyr_max_local(f)))
    return nyr_err(err, err_len,
                    "native NYIR verify: cannot build store scalar type map");
  for (size_t i = 0; i < f->len; ++i) {
    const nyr_inst_t *in = &f->data[i];
    if (in->op != NYIR_STORE_I64 || in->c < 0 ||
        (size_t)in->c >= types.value_count)
      continue;
    bool actual_f64 = types.value_f64[in->c];
    bool actual_f32 = types.value_f32[in->c];
    if (actual_f32 && actual_f64) {
      nyr_type_map_free(&types);
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
    nyr_type_map_free(&types);
    return nir_verify_inst_err(f, err, err_len, in, i, reason);
  }
  nyr_type_map_free(&types);
  return true;
}

/*
 * Ownership consistency (C2 ownership-invariant slice): a value's ownership
 * tag must agree through COPY chains, across local stores to the same slot,
 * and across PHI joins. A disagreement means a rewrite dropped or invented a
 * tag. Values with UNKNOWN tags on either side are skipped (unknown is the
 * conservative join).
 */
static const nyr_inst_t *nir_ownership_def_of(const nyr_func_t *f,
                                               int value) {
  if (value < 0)
    return NULL;
  for (size_t i = f->len; i > 0; --i) {
    if (f->data[i - 1].dst == value)
      return &f->data[i - 1];
  }
  return NULL;
}

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
            v >= 0 && v < f->next_value ? nir_ownership_def_of(f, v) : NULL;
        if (def && def->semantic_ownership != NY_SEM_OWN_UNKNOWN)
          other = v;
      }
    } else {
      continue;
    }
    if (own == NY_SEM_OWN_UNKNOWN || other < 0)
      continue;
    const nyr_inst_t *odef = nir_ownership_def_of(f, other);
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
                                                  char *err, size_t err_len) {
  nyr_type_map_t types = {0};
  if (!nyr_type_map_init(&types, f, nyr_max_local(f)))
    return nyr_err(err, err_len,
                    "native NYIR verify: cannot build local store type map");
  for (size_t i = 0; i < f->len; ++i) {
    const nyr_inst_t *in = &f->data[i];
    if (in->op != NYIR_STORE_LOCAL || in->a < 0 ||
        (size_t)in->a >= types.value_count)
      continue;
    bool actual_f64 = types.value_f64[in->a];
    bool actual_f32 = types.value_f32[in->a];
    if (actual_f64 && actual_f32) {
      nyr_type_map_free(&types);
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
    nyr_type_map_free(&types);
    return nir_verify_inst_err(f, err, err_len, in, i, reason);
  }
  nyr_type_map_free(&types);
  return true;
}

static bool nir_verify_return_scalar_classes(const nyr_func_t *f, char *err,
                                             size_t err_len) {
  bool has_return = false;
  for (size_t i = 0; i < f->len; ++i)
    has_return |= f->data[i].op == NYIR_RET;
  if (!has_return)
    return true;

  nyr_type_map_t types = {0};
  if (!nyr_type_map_init(&types, f, nyr_max_local(f)))
    return nyr_err(err, err_len,
                    "native NYIR verify: cannot build return scalar type map");
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
      nyr_type_map_free(&types);
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
    nyr_type_map_free(&types);
    return nir_verify_inst_err(f, err, err_len, in, i, reason);
  }
  nyr_type_map_free(&types);
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
            !nyr_cfg_dominates(&cfg,
                                cfg.inst_block[(size_t)definitions[value]],
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

bool nyr_ir_verify(const nyr_func_t *f, char *err, size_t err_len) {
  if (!f)
    return nyr_err(err, err_len, "native NYIR verify: missing function");
  if (f->next_value < 0)
    return nyr_err(err, err_len, "native NYIR verify: invalid value count");
  if (f->param_count && !f->param_types)
    return nyr_err(err, err_len,
                    "native NYIR verify: missing parameter types");
  bool has_aggregate_param = false;
  for (size_t i = 0; i < f->param_count; ++i) {
    if (f->param_types[i] > NYIR_PARAM_AGGREGATE)
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
         (in->flags & NYIR_INST_F_MEM_BYTE))) {
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
    case NYIR_VEC4_SHUFFLE_F64:
    case NYIR_VEC8_SHUFFLE_F32:
      if (in->dst < 0 || !nir_value_defined(f, defined, in->a)) {
        free(defined);
        return nir_verify_inst_err(f, err, err_len, in, i,
                                   "invalid unary value operand");
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
        bool has_definition = false;
        for (size_t j = 0; j < f->len; ++j)
          if (f->data[j].dst == v) {
            has_definition = true;
            break;
          }
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
    case NYIR_ALLOCA:
      if (in->dst < 0 || in->imm < 0) {
        free(defined);
        return nir_verify_inst_err(
            f, err, err_len, in, i,
            "alloca requires a valid destination and positive size");
      }
      break;
    case NYIR_COPY_STRUCT:
      if (in->a < 0 || in->b < 0 || in->imm < 0) {
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
      if (in->imm < 0 || !nir_value_defined(f, defined, in->a)) {
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
      if (!in->symbol || !in->symbol[0]) {
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
  if (!nir_verify_select_types(f, err, err_len))
    return false;
  if (!nir_verify_call_scalar_classes(f, err, err_len))
    return false;
  if (!nir_verify_store_scalar_classes(f, err, err_len))
    return false;
  if (!nir_verify_local_store_scalar_classes(f, err, err_len))
    return false;
  if (!nir_verify_return_scalar_classes(f, err, err_len))
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

bool nyr_analyze_binary_fold(nyr_op_t op, int64_t a, int64_t b,
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
                                   const nyr_value_fact_t *b,
                                   nyr_range_t *out) {
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

bool nyr_metadata_summary(const nyr_func_t *f,
                           nyr_metadata_summary_t *summary, char *err,
                           size_t err_len) {
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

bool nyr_validate_constraints(const nyr_func_t *f, char *err,
                               size_t err_len) {
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
