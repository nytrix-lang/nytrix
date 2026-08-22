/*
 * Advanced NYIR lowering: transforms high-level NYIR ops into
 * architecture-neutral optimized forms before machine-form conversion.
 */
#include "code/native/ir/internal.h"
#include "code/native/ir.h"
#include "code/native/ir/opt/util.h"
#include "base/compat.h"
#include "base/common.h"

#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Advanced optimizer seeds for the tiered-compiler roadmap.
 * Each pass is intentionally bounded, verifier-friendly, and gated by
 * opt level. Full research-scale versions stay future work.
 */

static bool nyir_advanced_fact_singleton(const nyir_value_fact_t *fact,
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

/*
 * Audited runtime-call effects used by optimization and tier diagnostics.
 *
 * Keep this table conservative: `CALL` is always present for a known helper;
 * `UNKNOWN_SIDE_EFFECT` is reserved for helpers whose externally observable
 * behavior cannot be represented by NYIR's current effect bits.  Read/write
 * helpers retain MAY_TRAP where an invalid raw pointer could fault.  This
 * gives alias/effect passes useful distinctions without pretending that raw
 * pointer accesses have provenance they do not carry.
 */
typedef struct {
  const char *name;
  unsigned effects;
} nyir_runtime_effect_entry_t;

static const nyir_runtime_effect_entry_t nyir_runtime_effects[] = {
    /*
     * Deterministic scalar helpers: no memory, allocation, or global state.
     */
    {"rt_native_i64_min", NYIR_EFFECT_CALL},
    {"rt_native_i64_max", NYIR_EFFECT_CALL},
    {"rt_native_f64_min", NYIR_EFFECT_CALL},
    {"rt_native_f64_max", NYIR_EFFECT_CALL},
    {"rt_native_bool_to_cstr", NYIR_EFFECT_CALL},
    {"rt_native_f64_to_i64", NYIR_EFFECT_CALL},
    {"rt_native_is_int", NYIR_EFFECT_CALL},

    /*
     * Stable process state / clock state.
     */
    {"rt_argc", NYIR_EFFECT_CALL | NYIR_EFFECT_READ_MEMORY},
    {"rt_ticks_ns", NYIR_EFFECT_CALL | NYIR_EFFECT_VOLATILE | NYIR_EFFECT_IO},

    /*
     * Read-only raw/managed storage helpers.
     */
    {"rt_native_cstr_len", NYIR_EFFECT_CALL | NYIR_EFFECT_READ_MEMORY |
                               NYIR_EFFECT_MAY_TRAP},
    {"rt_native_cstr_eq", NYIR_EFFECT_CALL | NYIR_EFFECT_READ_MEMORY |
                              NYIR_EFFECT_MAY_TRAP},
    {"rt_native_tbuf_len", NYIR_EFFECT_CALL | NYIR_EFFECT_READ_MEMORY |
                               NYIR_EFFECT_MAY_TRAP},
    {"rt_native_dict_get", NYIR_EFFECT_CALL | NYIR_EFFECT_READ_MEMORY |
                               NYIR_EFFECT_MAY_TRAP},
    {"rt_native_dict_has", NYIR_EFFECT_CALL | NYIR_EFFECT_READ_MEMORY |
                               NYIR_EFFECT_MAY_TRAP},
    {"rt_native_dict_len", NYIR_EFFECT_CALL | NYIR_EFFECT_READ_MEMORY |
                               NYIR_EFFECT_MAY_TRAP},
    {"rt_native_any_to_f64", NYIR_EFFECT_CALL | NYIR_EFFECT_READ_MEMORY |
                                 NYIR_EFFECT_MAY_TRAP},
    {"rt_native_bigint_cmp", NYIR_EFFECT_CALL | NYIR_EFFECT_READ_MEMORY |
                                 NYIR_EFFECT_MAY_TRAP},
    {"rt_bigint_to_i64_raw", NYIR_EFFECT_CALL | NYIR_EFFECT_READ_MEMORY |
                                NYIR_EFFECT_MAY_TRAP},
    {"rt_native_bigfloat_cmp", NYIR_EFFECT_CALL | NYIR_EFFECT_READ_MEMORY |
                                   NYIR_EFFECT_MAY_TRAP},
    {"rt_native_bigfloat_precision", NYIR_EFFECT_CALL | NYIR_EFFECT_READ_MEMORY |
                                         NYIR_EFFECT_MAY_TRAP},
    {"rt_native_bigfloat_to_f64", NYIR_EFFECT_CALL | NYIR_EFFECT_READ_MEMORY |
                                      NYIR_EFFECT_MAY_TRAP},

    /*
     * Allocation-only helpers whose inputs are raw scalars.
     */
    {"rt_native_tbuf_new", NYIR_EFFECT_CALL | NYIR_EFFECT_ALLOCATION},
    {"rt_native_dict_new", NYIR_EFFECT_CALL | NYIR_EFFECT_ALLOCATION},
    {"rt_native_i64_to_cstr", NYIR_EFFECT_CALL | NYIR_EFFECT_ALLOCATION},
    {"rt_bigint_from_i64_raw", NYIR_EFFECT_CALL | NYIR_EFFECT_ALLOCATION},

    /*
     * Allocation plus reads from input storage.
     */
    {"rt_native_cstr_concat", NYIR_EFFECT_CALL | NYIR_EFFECT_READ_MEMORY |
                                  NYIR_EFFECT_ALLOCATION | NYIR_EFFECT_MAY_TRAP},
    {"rt_native_cstr_builder_new", NYIR_EFFECT_CALL | NYIR_EFFECT_READ_MEMORY |
                                       NYIR_EFFECT_ALLOCATION |
                                       NYIR_EFFECT_MAY_TRAP},
    {"rt_native_cstr_replace", NYIR_EFFECT_CALL | NYIR_EFFECT_READ_MEMORY |
                                   NYIR_EFFECT_ALLOCATION | NYIR_EFFECT_MAY_TRAP},
    {"rt_native_any_to_cstr", NYIR_EFFECT_CALL | NYIR_EFFECT_READ_MEMORY |
                                  NYIR_EFFECT_ALLOCATION | NYIR_EFFECT_MAY_TRAP},
    {"rt_native_bigfloat_from_value", NYIR_EFFECT_CALL |
                                          NYIR_EFFECT_READ_MEMORY |
                                          NYIR_EFFECT_ALLOCATION |
                                          NYIR_EFFECT_MAY_TRAP},
    {"rt_native_bigfloat_pow_int", NYIR_EFFECT_CALL | NYIR_EFFECT_READ_MEMORY |
                                       NYIR_EFFECT_ALLOCATION |
                                       NYIR_EFFECT_MAY_TRAP},

    /*
     * BigInt and BigFloat values own heap-backed limbs. Arithmetic reads
     * operands and allocates a fresh result; division, parsing, conversion,
     * and root operations may also reject malformed or zero inputs.
     */
    {"rt_bigint_from_str", NYIR_EFFECT_CALL | NYIR_EFFECT_READ_MEMORY |
                               NYIR_EFFECT_ALLOCATION | NYIR_EFFECT_MAY_TRAP},
    {"rt_bigint_add", NYIR_EFFECT_CALL | NYIR_EFFECT_READ_MEMORY |
                          NYIR_EFFECT_ALLOCATION | NYIR_EFFECT_MAY_TRAP},
    {"rt_bigint_sub", NYIR_EFFECT_CALL | NYIR_EFFECT_READ_MEMORY |
                          NYIR_EFFECT_ALLOCATION | NYIR_EFFECT_MAY_TRAP},
    {"rt_bigint_mul", NYIR_EFFECT_CALL | NYIR_EFFECT_READ_MEMORY |
                          NYIR_EFFECT_ALLOCATION | NYIR_EFFECT_MAY_TRAP},
    {"rt_bigint_div", NYIR_EFFECT_CALL | NYIR_EFFECT_READ_MEMORY |
                          NYIR_EFFECT_ALLOCATION | NYIR_EFFECT_MAY_TRAP},
    {"rt_bigint_mod", NYIR_EFFECT_CALL | NYIR_EFFECT_READ_MEMORY |
                          NYIR_EFFECT_ALLOCATION | NYIR_EFFECT_MAY_TRAP},
    {"rt_bigfloat_zero", NYIR_EFFECT_CALL | NYIR_EFFECT_ALLOCATION},
    {"rt_bigfloat_one", NYIR_EFFECT_CALL | NYIR_EFFECT_ALLOCATION},
    {"rt_bigfloat_add", NYIR_EFFECT_CALL | NYIR_EFFECT_READ_MEMORY |
                            NYIR_EFFECT_ALLOCATION | NYIR_EFFECT_MAY_TRAP},
    {"rt_bigfloat_sub", NYIR_EFFECT_CALL | NYIR_EFFECT_READ_MEMORY |
                            NYIR_EFFECT_ALLOCATION | NYIR_EFFECT_MAY_TRAP},
    {"rt_bigfloat_mul", NYIR_EFFECT_CALL | NYIR_EFFECT_READ_MEMORY |
                            NYIR_EFFECT_ALLOCATION | NYIR_EFFECT_MAY_TRAP},
    {"rt_bigfloat_div", NYIR_EFFECT_CALL | NYIR_EFFECT_READ_MEMORY |
                            NYIR_EFFECT_ALLOCATION | NYIR_EFFECT_MAY_TRAP},
    {"rt_bigfloat_sqrt", NYIR_EFFECT_CALL | NYIR_EFFECT_READ_MEMORY |
                             NYIR_EFFECT_ALLOCATION | NYIR_EFFECT_MAY_TRAP},
    {"rt_bigfloat_neg", NYIR_EFFECT_CALL | NYIR_EFFECT_READ_MEMORY |
                            NYIR_EFFECT_ALLOCATION | NYIR_EFFECT_MAY_TRAP},
    {"rt_bigfloat_abs", NYIR_EFFECT_CALL | NYIR_EFFECT_READ_MEMORY |
                            NYIR_EFFECT_ALLOCATION | NYIR_EFFECT_MAY_TRAP},

    /*
     * Mutating helpers. Some may grow storage and therefore allocate.
     */
    {"rt_native_tbuf_append", NYIR_EFFECT_CALL | NYIR_EFFECT_READ_MEMORY |
                                  NYIR_EFFECT_WRITE_MEMORY |
                                  NYIR_EFFECT_ALLOCATION | NYIR_EFFECT_MAY_TRAP},
    {"rt_native_tbuf_append_i64", NYIR_EFFECT_CALL | NYIR_EFFECT_READ_MEMORY |
                                      NYIR_EFFECT_WRITE_MEMORY |
                                      NYIR_EFFECT_ALLOCATION |
                                      NYIR_EFFECT_MAY_TRAP},
    {"rt_native_tbuf_pop", NYIR_EFFECT_CALL | NYIR_EFFECT_READ_MEMORY |
                               NYIR_EFFECT_WRITE_MEMORY | NYIR_EFFECT_MAY_TRAP},
    {"rt_native_dict_set", NYIR_EFFECT_CALL | NYIR_EFFECT_READ_MEMORY |
                               NYIR_EFFECT_WRITE_MEMORY |
                               NYIR_EFFECT_ALLOCATION | NYIR_EFFECT_MAY_TRAP},
    {"rt_native_dict_delete", NYIR_EFFECT_CALL | NYIR_EFFECT_READ_MEMORY |
                                  NYIR_EFFECT_WRITE_MEMORY |
                                  NYIR_EFFECT_MAY_TRAP},
    {"rt_native_cstr_builder_append", NYIR_EFFECT_CALL |
                                          NYIR_EFFECT_READ_MEMORY |
                                          NYIR_EFFECT_WRITE_MEMORY |
                                          NYIR_EFFECT_ALLOCATION |
                                          NYIR_EFFECT_MAY_TRAP},
    {"rt_native_cstr_builder_finalize", NYIR_EFFECT_CALL |
                                            NYIR_EFFECT_READ_MEMORY |
                                            NYIR_EFFECT_WRITE_MEMORY |
                                            NYIR_EFFECT_MAY_TRAP},
    /*
     * Tag tests update private runtime lookup caches in some cases.
     */
    {"rt_native_has_tag", NYIR_EFFECT_CALL | NYIR_EFFECT_READ_MEMORY |
                              NYIR_EFFECT_WRITE_MEMORY | NYIR_EFFECT_MAY_TRAP},

    /*
     * Dedicated externally observable effect classes keep these calls
     * conservative without collapsing unrelated memory into UNKNOWN.
     */
    {"rt_native_sin_f64", NYIR_EFFECT_CALL | NYIR_EFFECT_FENV},
    {"rt_native_cos_f64", NYIR_EFFECT_CALL | NYIR_EFFECT_FENV},
    {"rt_native_fmod_f64", NYIR_EFFECT_CALL | NYIR_EFFECT_FENV},
    {"rt_native_assert_cstr", NYIR_EFFECT_CALL | NYIR_EFFECT_READ_MEMORY |
                                  NYIR_EFFECT_MAY_TRAP | NYIR_EFFECT_IO},
    {"rt_native_thread_spawn_raw", NYIR_EFFECT_CALL | NYIR_EFFECT_ALLOCATION |
                                       NYIR_EFFECT_THREAD},
    {"rt_print_cstr", NYIR_EFFECT_CALL | NYIR_EFFECT_READ_MEMORY |
                         NYIR_EFFECT_MAY_TRAP | NYIR_EFFECT_IO},
    {"rt_print_i64_raw", NYIR_EFFECT_CALL | NYIR_EFFECT_IO},
    {"rt_print_f64_raw", NYIR_EFFECT_CALL | NYIR_EFFECT_IO | NYIR_EFFECT_FENV},
    {"rt_print_newline", NYIR_EFFECT_CALL | NYIR_EFFECT_IO},
};

unsigned nyir_call_effect_summary(const nyir_inst_t *inst) {
  if (!inst || inst->op != NYIR_CALL)
    return NYIR_EFFECT_NONE;
  const char *name = inst->symbol;
  if (!name || !name[0])
    return NYIR_EFFECT_CALL | NYIR_EFFECT_FFI | NYIR_EFFECT_UNKNOWN_SIDE_EFFECT;
  for (size_t i = 0;
       i < sizeof(nyir_runtime_effects) / sizeof(nyir_runtime_effects[0]);
       ++i)
    if (strcmp(name, nyir_runtime_effects[i].name) == 0)
      return nyir_runtime_effects[i].effects;
  return NYIR_EFFECT_CALL | NYIR_EFFECT_FFI | NYIR_EFFECT_UNKNOWN_SIDE_EFFECT;
}

unsigned nyir_effective_effects(const nyir_inst_t *inst) {
  if (!inst)
    return NYIR_EFFECT_NONE;
  if (inst->op == NYIR_CALL) {
    /*
     * The native builder attaches a verified interprocedural summary to
     * calls of user functions (NYIR_INST_F_EFFECTS_KNOWN).  Trust it over
     * the runtime-symbol table, which would otherwise report
     * CALL|FFI|UNKNOWN_SIDE_EFFECT for every user call and defeat LICM.
     */
    if (inst->flags & NYIR_INST_F_EFFECTS_KNOWN)
      return inst->effects;
    return nyir_call_effect_summary(inst);
  }
  return inst->effects | nyir_inst_effects(inst);
}

/*
 * Effect / alias summary: collapse per-instruction effects into one mask.
 */
bool nyir_effect_summary(const nyir_func_t *f, unsigned *out_mask,
                           size_t *out_reads, size_t *out_writes,
                           size_t *out_calls) {
  if (!f || !out_mask)
    return false;
  unsigned mask = 0;
  size_t reads = 0, writes = 0, calls = 0;
  for (size_t i = 0; i < f->len; ++i) {
    unsigned e = nyir_effective_effects(&f->data[i]);
    mask |= e;
    if (e & (NYIR_EFFECT_READ_LOCAL | NYIR_EFFECT_READ_MEMORY))
      reads++;
    if (e & (NYIR_EFFECT_WRITE_LOCAL | NYIR_EFFECT_WRITE_MEMORY))
      writes++;
    if (e & NYIR_EFFECT_CALL)
      calls++;
  }
  *out_mask = mask;
  if (out_reads)
    *out_reads = reads;
  if (out_writes)
    *out_writes = writes;
  if (out_calls)
    *out_calls = calls;
  return true;
}

/*
 * Null/alignment fact seed: after `x != 0` / `x > 0` compare+branch, mark the
 * compared value's range as excluding zero when the true edge is taken. We
 * only strengthen instruction-local range metadata on the compare result and
 * on proven non-zero AND masks (low-bit alignment).
 */
bool nyir_null_align_facts(nyir_func_t *f) {
  if (!f || f->next_value <= 0)
    return true;
  nyir_value_fact_t *facts =
      ny_calloc_array((size_t)f->next_value, sizeof(*facts));
  if (!facts || !nyir_analyze_values(f, facts, (size_t)f->next_value, NULL, 0)) {
    free(facts);
    return false;
  }
  for (size_t i = 0; i < f->len; ++i) {
    nyir_inst_t *in = &f->data[i];
    /*
     * (x & (align-1)) == 0 for power-of-two align → alignment fact on range of
     * the AND result is already 0..mask; nothing to rewrite. Strengthen known
     * non-null: if a value is proven > 0 by range, attach min=1 when missing.
     */
    if (in->dst >= 0 && in->dst < f->next_value) {
      const nyir_range_t *r = &facts[in->dst].range;
      if (r->has_min && r->min > 0 && !in->range.has_min) {
        in->range.has_min = true;
        in->range.min = r->min;
        if (r->has_max) {
          in->range.has_max = true;
          in->range.max = r->max;
        }
      }
    }
    /*
     * Known-bits upper bound: x & non-negative mask is always in
     * [0, mask], even when the mask is not the all-low-bits form.
     * This is the useful fact for parity/bit-field consumers.
     */
    if (in->op == NYIR_AND_I64 && in->b >= 0 && in->b < f->next_value &&
        facts[in->b].known_const && facts[in->b].const_value >= 0) {
      int64_t mask = facts[in->b].const_value;
      in->range.has_min = true;
      in->range.has_max = true;
      in->range.min = 0;
      in->range.max = mask;
    }
    int64_t rhs_singleton = 0;
    bool rhs_is_singleton =
        in->b >= 0 && in->b < f->next_value &&
        nyir_advanced_fact_singleton(&facts[in->b], &rhs_singleton);
    if (in->op == NYIR_MOD_I64 && rhs_is_singleton && rhs_singleton != 0 &&
        rhs_singleton != INT64_MIN && facts[in->a].range.has_min &&
        facts[in->a].range.min >= 0) {
      int64_t mod = rhs_singleton < 0 ? -rhs_singleton : rhs_singleton;
      in->range.has_min = true;
      in->range.has_max = true;
      in->range.min = 0;
      in->range.max = mod - 1;
      /*
       * A non-negative dividend that cannot reach the modulus is unchanged
       * by the operation (a % m == a), so keep the tighter dividend bound
       * instead of widening to m-1.
       */
      if (facts[in->a].range.has_max && facts[in->a].range.max < mod - 1)
        in->range.max = facts[in->a].range.max;
    }
    if (in->op == NYIR_SAR_I64 && rhs_is_singleton && rhs_singleton >= 0 &&
        rhs_singleton < 64 && facts[in->a].range.has_min &&
        facts[in->a].range.min >= 0 && facts[in->a].range.has_max) {
      unsigned shift = (unsigned)rhs_singleton;
      in->range.has_min = true;
      in->range.has_max = true;
      in->range.min = facts[in->a].range.min >> shift;
      in->range.max = facts[in->a].range.max >> shift;
    }
    if (in->op == NYIR_SHL_I64 && rhs_is_singleton && rhs_singleton >= 0 &&
        rhs_singleton < 63 && facts[in->a].range.has_min &&
        facts[in->a].range.min >= 0 && facts[in->a].range.has_max &&
        facts[in->a].range.max <= (INT64_MAX >> (unsigned)rhs_singleton)) {
      unsigned shift = (unsigned)rhs_singleton;
      in->range.has_min = true;
      in->range.has_max = true;
      in->range.min = facts[in->a].range.min << shift;
      in->range.max = facts[in->a].range.max << shift;
    }
  }
  free(facts);
  return true;
}

/*
 * Induction simplification seed: i = i + C with C const → keep as add (already
 * strength-reduced when C is power-of-two related). Detect `x - x` style and
 * `phi` self-add of constant step already handled by SCCP/peephole. Here we
 * fold `(i + c) - c` and `(i - c) + c` via local e-graph style peep.
 */
bool nyir_iv_simplify(nyir_func_t *f) {
  if (!f || f->len < 2)
    return true;
  for (size_t i = 0; i < f->len; ++i) {
    nyir_inst_t *in = &f->data[i];
    if (in->dst < 0 || in->a < 0 || in->b < 0)
      continue;
    /*
     * (x + c) - c  or  (x - c) + c → x when c is the same SSA const use.
     */
    if (in->op != NYIR_ADD_I64 && in->op != NYIR_SUB_I64)
      continue;
    /*
     * Look for def of a as ADD/SUB with same const b.
     */
    for (size_t j = i; j > 0; --j) {
      const nyir_inst_t *def = &f->data[j - 1];
      if (def->dst != in->a)
        continue;
      if (def->op == NYIR_ADD_I64 && in->op == NYIR_SUB_I64 &&
          def->b == in->b && def->b >= 0) {
        nir_make_copy(in, def->a);
      } else if (def->op == NYIR_SUB_I64 && in->op == NYIR_ADD_I64 &&
                 def->b == in->b && def->b >= 0) {
        nir_make_copy(in, def->a);
      }
      break;
    }
  }
  return true;
}

static int nyir_value_skip_copies(const nyir_func_t *f, const int *defs,
                                  int value) {
  for (int depth = 0; f && defs && value >= 0 && value < f->next_value &&
                      depth < 16; ++depth) {
    int def_i = defs[value];
    if (def_i < 0)
      break;
    const nyir_inst_t *def = &f->data[def_i];
    if (def->op != NYIR_COPY || def->a < 0)
      break;
    value = def->a;
  }
  return value;
}

static bool nyir_const_i64_value(const nyir_func_t *f, const int *defs,
                                 int value, int64_t *out) {
  value = nyir_value_skip_copies(f, defs, value);
  if (!f || !defs || value < 0 || value >= f->next_value)
    return false;
  int def_i = defs[value];
  if (def_i < 0)
    return false;
  const nyir_inst_t *def = &f->data[def_i];
  if (def->op != NYIR_CONST_I64)
    return false;
  if (out)
    *out = def->imm;
  return true;
}

static bool nyir_scaled_base(const nyir_func_t *f, const int *defs, int value,
                             int *base_out, unsigned *scale_out) {
  value = nyir_value_skip_copies(f, defs, value);
  if (!f || !defs || value < 0 || value >= f->next_value)
    return false;
  int def_i = defs[value];
  if (def_i >= 0) {
    const nyir_inst_t *def = &f->data[def_i];
    int64_t scale = 0;
    if (def->op == NYIR_SHL_I64 &&
        nyir_const_i64_value(f, defs, def->b, &scale) && scale >= 0 &&
        scale < 31) {
      *base_out = nyir_value_skip_copies(f, defs, def->a);
      *scale_out = (unsigned)1u << (unsigned)scale;
      return true;
    }
    if (def->op == NYIR_MUL_I64) {
      if (nyir_const_i64_value(f, defs, def->a, &scale) &&
          scale > 0 && (uint64_t)scale <= UINT_MAX) {
        *base_out = nyir_value_skip_copies(f, defs, def->b);
        *scale_out = (unsigned)scale;
        return true;
      }
      if (nyir_const_i64_value(f, defs, def->b, &scale) &&
          scale > 0 && (uint64_t)scale <= UINT_MAX) {
        *base_out = nyir_value_skip_copies(f, defs, def->a);
        *scale_out = (unsigned)scale;
        return true;
      }
    }
  }
  *base_out = value;
  *scale_out = 1;
  return true;
}

static bool nyir_values_same_at(const nyir_func_t *f, const int *defs, int a,
                                int b, size_t at) {
  a = nyir_value_skip_copies(f, defs, a);
  b = nyir_value_skip_copies(f, defs, b);
  return (a >= 0 && a == b) || nir_operands_same_value(f, a, b, at);
}

static bool nyir_value_is_minus_one_from(const nyir_func_t *f, const int *defs,
                                         int value, int base, size_t at) {
  value = nyir_value_skip_copies(f, defs, value);
  base = nyir_value_skip_copies(f, defs, base);
  if (!f || !defs || value < 0 || value >= f->next_value)
    return false;
  int def_i = defs[value];
  if (def_i < 0)
    return false;
  const nyir_inst_t *def = &f->data[def_i];
  int64_t c = 0;
  if (def->op == NYIR_SUB_I64 &&
      nyir_values_same_at(f, defs, def->a, base, at) &&
      nyir_const_i64_value(f, defs, def->b, &c) && c == 1)
    return true;
  if (def->op == NYIR_ADD_I64 &&
      nyir_values_same_at(f, defs, def->a, base, at) &&
      nyir_const_i64_value(f, defs, def->b, &c) && c == -1)
    return true;
  if (def->op == NYIR_ADD_I64 &&
      nyir_values_same_at(f, defs, def->b, base, at) &&
      nyir_const_i64_value(f, defs, def->a, &c) && c == -1)
    return true;
  return false;
}

static size_t nyir_previous_non_nop(const nyir_func_t *f, size_t i) {
  while (i > 0) {
    --i;
    if (f->data[i].op != NYIR_NOP)
      return i;
  }
  return SIZE_MAX;
}

static size_t nyir_label_pred_count(const nyir_func_t *f, int label,
                                    size_t label_i) {
  size_t count = 0;
  for (size_t i = 0; f && i < f->len; ++i) {
    const nyir_inst_t *in = &f->data[i];
    if ((in->op == NYIR_BR || in->op == NYIR_BR_IF) && in->imm == label)
      ++count;
  }
  size_t prev = nyir_previous_non_nop(f, label_i);
  if (prev != SIZE_MAX) {
    nyir_op_t op = f->data[prev].op;
    if (op != NYIR_BR && op != NYIR_RET)
      ++count;
  }
  return count;
}

static bool nyir_cmp_proves_index_in_len(const nyir_func_t *f,
                                         const int *defs,
                                         const nyir_inst_t *cmp,
                                         bool true_edge, int index, int len,
                                         size_t at) {
  if (!cmp || cmp->op != NYIR_CMP_I64)
    return false;
  int lhs = nyir_value_skip_copies(f, defs, cmp->a);
  int rhs = nyir_value_skip_copies(f, defs, cmp->b);
  index = nyir_value_skip_copies(f, defs, index);
  len = nyir_value_skip_copies(f, defs, len);
  if (!nyir_values_same_at(f, defs, lhs, index, at))
    return false;
  if (true_edge) {
    if (cmp->cmp == NYIR_CMP_LT &&
        nyir_values_same_at(f, defs, rhs, len, at))
      return true;
    return cmp->cmp == NYIR_CMP_LE &&
           nyir_value_is_minus_one_from(f, defs, rhs, len, at);
  }
  if (cmp->cmp == NYIR_CMP_GE &&
      nyir_values_same_at(f, defs, rhs, len, at))
    return true;
  return cmp->cmp == NYIR_CMP_GT &&
         nyir_value_is_minus_one_from(f, defs, rhs, len, at);
}

static const nyir_inst_t *nyir_branch_cmp(const nyir_func_t *f,
                                          const int *defs,
                                          const nyir_inst_t *br) {
  if (!f || !defs || !br || br->op != NYIR_BR_IF)
    return NULL;
  int cond = nyir_value_skip_copies(f, defs, br->a);
  if (cond < 0 || cond >= f->next_value)
    return NULL;
  int def_i = defs[cond];
  return def_i >= 0 ? &f->data[def_i] : NULL;
}

static bool nyir_guard_proves_index_in_len(const nyir_func_t *f,
                                           const int *defs,
                                           size_t label_i, int label,
                                           int index, int len, size_t at) {
  if (nyir_label_pred_count(f, label, label_i) != 1)
    return false;
  size_t prev = nyir_previous_non_nop(f, label_i);
  if (prev != SIZE_MAX) {
    const nyir_inst_t *br = &f->data[prev];
    if (br->op == NYIR_BR_IF && br->imm != label) {
      const nyir_inst_t *cmp = nyir_branch_cmp(f, defs, br);
      if (nyir_cmp_proves_index_in_len(f, defs, cmp, false, index, len, at))
        return true;
    }
  }
  for (size_t i = 0; i < f->len; ++i) {
    const nyir_inst_t *br = &f->data[i];
    if (br->op != NYIR_BR_IF || br->imm != label)
      continue;
    const nyir_inst_t *cmp = nyir_branch_cmp(f, defs, br);
    return nyir_cmp_proves_index_in_len(f, defs, cmp, true, index, len, at);
  }
  return false;
}

static bool nyir_bounds_check_guard_safe(const nyir_func_t *f, const int *defs,
                                         size_t check_i, size_t label_i,
                                         int label) {
  const nyir_inst_t *check = &f->data[check_i];
  if (check->op != NYIR_BOUNDS_CHECK || check->c < 0)
    return false;
  int index = -1, len = -1;
  unsigned index_shift = 0, len_shift = 0;
  if (!nyir_scaled_base(f, defs, check->b, &index, &index_shift) ||
      !nyir_scaled_base(f, defs, check->c, &len, &len_shift) ||
      index_shift != len_shift)
    return false;
  return nyir_guard_proves_index_in_len(f, defs, label_i, label, index, len,
                                        check_i);
}

static bool nyir_bce_trace_enabled(void) {
  static int initialized = 0;
  static bool enabled = false;
  if (!initialized) {
    const char *v = getenv("NY_TRACE_BCE");
    enabled = v && v[0] && strcmp(v, "0") != 0;
    initialized = 1;
  }
  return enabled;
}

static void nyir_bce_trace(const nyir_func_t *f, size_t pc,
                           const char *status, const char *reason);

static bool nyir_bounds_check_same_operands(const nyir_inst_t *a,
                                            const nyir_inst_t *b) {
  return a && b && a->op == NYIR_BOUNDS_CHECK && b->op == NYIR_BOUNDS_CHECK &&
         a->a == b->a && a->b == b->b && a->c == b->c && a->imm == b->imm;
}

/*
 * Remove a bounds check when an identical check dominates it.  SSA operands
 * cannot change between the two checks, and reaching the later check proves
 * the earlier one already succeeded, so this preserves both safety and trap
 * ordering while avoiding repeated checks across common CFG joins/blocks.
 */
static bool nyir_remove_dominated_bounds_checks(nyir_func_t *f) {
  if (!f || f->len == 0)
    return true;
  nyir_cfg_t cfg = {0};
  if (!nyir_cfg_build(f, &cfg))
    return false;
  for (size_t i = 0; i < f->len; ++i) {
    nyir_inst_t *check = &f->data[i];
    if (check->op != NYIR_BOUNDS_CHECK)
      continue;
    size_t use_block = cfg.inst_block[i];
    for (size_t j = 0; j < i; ++j) {
      const nyir_inst_t *prior = &f->data[j];
      if (!nyir_bounds_check_same_operands(prior, check))
        continue;
      size_t def_block = cfg.inst_block[j];
      if (!nyir_cfg_dominates(&cfg, def_block, use_block))
        continue;
      nyir_bce_trace(f, i, "eliminate",
                     "identical earlier bounds check dominates this check");
      nyir_inst_discard(check);
      break;
    }
  }
  nyir_cfg_free(&cfg);
  return true;
}

static void nyir_bce_trace(const nyir_func_t *f, size_t pc,
                           const char *status, const char *reason) {
  if (!nyir_bce_trace_enabled())
    return;
  const nyir_inst_t *in = f && pc < f->len ? &f->data[pc] : NULL;
  const char *file = in && in->debug.file ? in->debug.file : "<unknown>";
  fprintf(stderr,
          "nyir bce: %s pc=%zu at %s:%u:%u block_heat=%" PRIu64
          " loop_heat=%" PRIu64 " reason=%s\n",
          status ? status : "remark", pc, file,
          in ? in->debug.line : 0, in ? in->debug.column : 0,
          ny_native_profile_block_hot(pc), ny_native_profile_loop_hot(pc),
          reason ? reason : "none");
}

/*
 * Bounds-check elimination. The first case folds simple masked comparisons.
 * The second case removes lowered list/byte-buffer checks on the taken or
 * fall-through edge of a single-predecessor loop guard (`if i < len goto body`,
 * `if i <= len - 1 goto body`, `if i >= len goto exit`, or
 * `if i > len - 1 goto exit`) when the checked offset and dynamic byte length
 * share the same scale.
 */
bool nyir_bounds_check_elim(nyir_func_t *f) {
  if (!f || f->next_value <= 0)
    return true;
  bool *known = calloc((size_t)f->next_value, sizeof(bool));
  int64_t *value = calloc((size_t)f->next_value, sizeof(int64_t));
  int *defs = nyir_build_defs(f);
  nyir_value_fact_t *facts =
      calloc((size_t)f->next_value, sizeof(*facts));
  if (!known || !value || !defs || !facts) {
    free(known);
    free(value);
    free(defs);
    free(facts);
    return false;
  }
  bool have_facts =
      nyir_analyze_values(f, facts, (size_t)f->next_value, NULL, 0);
  if (!nyir_remove_dominated_bounds_checks(f)) {
    free(known);
    free(value);
    free(defs);
    free(facts);
    return false;
  }
  size_t current_label_i = SIZE_MAX;
  int current_label = -1;
  for (size_t i = 0; i < f->len; ++i) {
    nyir_inst_t *in = &f->data[i];
    if (in->op == NYIR_LABEL) {
      current_label_i = i;
      current_label = (int)in->imm;
    }
    if (in->op == NYIR_BOUNDS_CHECK && current_label_i != SIZE_MAX &&
        nyir_bounds_check_guard_safe(f, defs, i, current_label_i,
                                     current_label)) {
      nyir_bce_trace(f, i, "eliminate",
                     "dominating loop guard proves index within length");
      *in = (nyir_inst_t){.op = NYIR_NOP, .dst = -1, .a = -1, .b = -1,
                          .c = -1, .d = -1, .e = -1, .f = -1};
      continue;
    }
    if (in->op == NYIR_BOUNDS_CHECK && have_facts && in->b >= 0 &&
        in->b < f->next_value) {
      const nyir_range_t *off = &facts[in->b].range;
      bool proven = off->has_min && off->has_max && off->min >= 0;
      if (proven && in->c >= 0 && in->c < f->next_value) {
        const nyir_range_t *len = &facts[in->c].range;
        proven = len->has_min && len->min > 0 && off->max < len->min;
      } else if (proven && in->c < 0) {
        proven = in->imm > 0 && off->max < in->imm;
      } else {
        proven = false;
      }
      if (proven) {
        nyir_bce_trace(f, i, "eliminate",
                       "retained range facts prove byte offset within length");
        nyir_inst_discard(in);
        continue;
      }
    }
    if (in->op == NYIR_CONST_I64 && in->dst >= 0 &&
        in->dst < f->next_value) {
      known[in->dst] = true;
      value[in->dst] = in->imm;
    }
    if (in->op != NYIR_CMP_I64 || in->a < 0 || in->b < 0)
      continue;
    if (in->cmp != NYIR_CMP_LT && in->cmp != NYIR_CMP_LE)
      continue;
    if (!known[in->b])
      continue;
    int64_t lim = value[in->b];
    /*
     * Find if a is AND with const mask.
     */
    for (size_t j = i; j > 0; --j) {
      const nyir_inst_t *def = &f->data[j - 1];
      if (def->dst != in->a)
        continue;
      if (def->op == NYIR_AND_I64 && def->b >= 0 && known[def->b]) {
        int64_t mask = value[def->b];
        if (mask >= 0 && lim > 0 &&
            (in->cmp == NYIR_CMP_LT ? mask < lim : mask <= lim)) {
          *in = (nyir_inst_t){.op = NYIR_CONST_I64,
                              .dst = in->dst,
                              .a = -1,
                              .b = -1,
                              .imm = 1,
                              .range = {.has_min = true,
                                        .has_max = true,
                                        .min = 1,
                                        .max = 1}};
        }
      }
      break;
    }
  }
  if (nyir_bce_trace_enabled()) {
    for (size_t i = 0; i < f->len; ++i) {
      const nyir_inst_t *check = &f->data[i];
      if (check->op != NYIR_BOUNDS_CHECK)
        continue;
      const char *reason =
          "no recognized dominating guard discharges the check";
      if (have_facts && check->b >= 0 && check->b < f->next_value &&
          (!facts[check->b].range.has_min ||
           !facts[check->b].range.has_max)) {
        reason = "checked offset has no retained range fact";
      } else if (have_facts && check->b >= 0 && check->b < f->next_value &&
                 facts[check->b].range.has_min &&
                 facts[check->b].range.min < 0) {
        reason = "checked offset may be negative";
      } else if (check->c >= 0 && have_facts && check->c < f->next_value &&
                 (!facts[check->c].range.has_min ||
                  !facts[check->c].range.has_max)) {
        reason = "dynamic length has no retained range fact";
      } else if (check->c >= 0 && have_facts && check->b >= 0 &&
                 check->b < f->next_value && check->c < f->next_value &&
                 facts[check->b].range.has_max &&
                 facts[check->c].range.has_min &&
                 facts[check->b].range.max >= facts[check->c].range.min) {
        reason = "offset maximum is not proven below the length minimum";
      } else if (check->c < 0 && check->imm <= 0) {
        reason = "check has no positive static or dynamic length proof";
      }
      nyir_bce_trace(f, i, "retain", reason);
    }
  }
  free(facts);
  free(defs);
  free(known);
  free(value);
  return true;
}

/*
 * Local e-graph seed: reassociate pure (a+b)+c chains and commute consts to
 * the right on ADD/MUL for CSE friendliness. Bounded single forward pass.
 */
bool nyir_egraph_local(nyir_func_t *f) {
  if (!f || f->next_value <= 0)
    return true;
  int *defs = nyir_build_defs(f);
  if (!defs)
    return false;
  for (size_t i = 0; i < f->len; ++i) {
    nyir_inst_t *in = &f->data[i];
    if ((in->op != NYIR_ADD_I64 && in->op != NYIR_MUL_I64) || in->a < 0 ||
        in->b < 0)
      continue;
    int ad = in->a < f->next_value ? defs[in->a] : -1;
    int bd = in->b < f->next_value ? defs[in->b] : -1;
    bool a_const = ad >= 0 && (size_t)ad < i &&
                   f->data[ad].op == NYIR_CONST_I64;
    bool b_const = bd >= 0 && (size_t)bd < i &&
                   f->data[bd].op == NYIR_CONST_I64;
    /*
     * Canonicalize commutative scalar ops with constants on the right.
     */
    if (a_const && !b_const) {
      int t = in->a;
      in->a = in->b;
      in->b = t;
    }
  }
  free(defs);
  return true;
}

/*
 * apply_rules / ny_isle_apply_nir live in isle.c (shared NYIR + machine form).
 */

/*
 * Small pure-function inliner: replace CALL to a known callee with a copy of
 * its straight-line body when body is tiny and pure.
 */
static const nyir_inline_callee_t *nyir_inline_callees;
static size_t nyir_inline_callee_count;

void nyir_set_inline_callees(const nyir_inline_callee_t *callees,
                               size_t count) {
  nyir_inline_callees = callees;
  nyir_inline_callee_count = count;
}

static const nyir_func_t *nyir_find_inline_callee(const char *name) {
  if (!name)
    return NULL;
  for (size_t i = 0; i < nyir_inline_callee_count; ++i) {
    const char *cname = nyir_inline_callees[i].name;
    if (!cname)
      continue;
    if (strcmp(cname, name) == 0)
      return nyir_inline_callees[i].func;
    const char *dot = strrchr(cname, '.');
    if (dot && strcmp(dot + 1, name) == 0)
      return nyir_inline_callees[i].func;
    const char *ndot = strrchr(name, '.');
    if (ndot && strcmp(cname, ndot + 1) == 0)
      return nyir_inline_callees[i].func;
    if (strncmp(name, "ny_fn_", 6) == 0 && strcmp(cname, name + 6) == 0)
      return nyir_inline_callees[i].func;
    if (strncmp(cname, "ny_fn_", 6) == 0 && strcmp(cname + 6, name) == 0)
      return nyir_inline_callees[i].func;
  }
  return NULL;
}

static bool nyir_func_is_inline_candidate(const nyir_func_t *f) {
  if (!f || f->len == 0 || f->len > 32)
    return false;
  if (f->len > 12 && !ny_native_profile_should_inline(f->len))
    return false;
  for (size_t i = 0; i < f->len; ++i) {
    const nyir_inst_t *in = &f->data[i];
    if (in->op == NYIR_CALL || in->op == NYIR_PHI ||
        in->op == NYIR_BR || in->op == NYIR_BR_IF ||
        (in->op == NYIR_LABEL && i > 0))
      return false;
    if (in->op == NYIR_STORE_LOCAL &&
        (in->imm < 0 || in->imm >= (int64_t)f->param_count))
      return false;
  }
  return true;
}

static bool nyir_is_i64_binop(nyir_op_t op) {
  return op == NYIR_ADD_I64 || op == NYIR_MUL_I64 || op == NYIR_SUB_I64 ||
         op == NYIR_AND_I64 || op == NYIR_OR_I64 || op == NYIR_XOR_I64 ||
         op == NYIR_SHL_I64 || op == NYIR_SAR_I64 || op == NYIR_DIV_I64 ||
         op == NYIR_MOD_I64;
}

/*
 * Ensure room for `extra` more instructions at index `at` (shifts tail).
 */
static bool nyir_ensure_insert(nyir_func_t *f, size_t at, size_t extra) {
  if (!f || extra == 0)
    return true;
  if (f->len + extra > f->cap) {
    size_t nc = f->cap ? f->cap * 2 : 64;
    while (nc < f->len + extra)
      nc *= 2;
    nyir_inst_t *nd = ny_realloc_array(f->data, nc, sizeof(*nd));
    if (!nd)
      return false;
    f->data = nd;
    f->cap = nc;
  }
  memmove(&f->data[at + extra], &f->data[at],
          (f->len - at) * sizeof(nyir_inst_t));
  f->len += extra;
  return true;
}

bool nyir_inline_small(nyir_func_t *f) {
  if (!f || nyir_inline_callee_count == 0)
    return true;
  for (size_t i = 0; i < f->len; ++i) {
    nyir_inst_t *in = &f->data[i];
    if (in->op != NYIR_CALL || !in->symbol)
      continue;
    const nyir_func_t *callee = nyir_find_inline_callee(in->symbol);
    if (!callee || !nyir_func_is_inline_candidate(callee))
      continue;

    int64_t ret_imm = 0;
    bool ret_const = false;
    int ret_src = -1;
    for (size_t k = 0; k < callee->len; ++k) {
      const nyir_inst_t *c = &callee->data[k];
      if (c->op != NYIR_RET || c->a < 0)
        continue;
      ret_src = c->a;
      for (size_t j = 0; j < k; ++j) {
        if (callee->data[j].op == NYIR_CONST_I64 &&
            callee->data[j].dst == c->a) {
          ret_imm = callee->data[j].imm;
          ret_const = true;
        }
      }
    }
    if (ret_const && in->dst >= 0) {
      *in = (nyir_inst_t){.op = NYIR_CONST_I64,
                            .dst = in->dst,
                            .imm = ret_imm,
                            .range = {.has_min = true,
                                      .has_max = true,
                                      .min = ret_imm,
                                      .max = ret_imm}};
      continue;
    }

    /*
     * Single-expression callees: load.local 0; op; ret — specialize when
     * one arg and pure unary/binary on that arg only.
     */
    if (callee->len == 3 && in->a >= 0 && in->dst >= 0 &&
        callee->data[0].op == NYIR_LOAD_LOCAL && callee->data[0].imm == 0 &&
        callee->data[2].op == NYIR_RET &&
        callee->data[2].a == callee->data[1].dst) {
      const nyir_inst_t *op = &callee->data[1];
      if (nyir_is_i64_binop(op->op)) {
        /*
         * Need const second operand in callee.
         */
        int64_t imm = 0;
        bool has_imm = false;
        if (op->a == callee->data[0].dst && op->b >= 0) {
          for (size_t j = 0; j < callee->len; ++j)
            if (callee->data[j].op == NYIR_CONST_I64 &&
                callee->data[j].dst == op->b) {
              imm = callee->data[j].imm;
              has_imm = true;
            }
        }
        if (has_imm) {
          /*
           * Expand call to: const imm; op arg, imm → dst
           */
          if (!nyir_ensure_insert(f, i, 1))
            return false;
          int cdst = f->next_value++;
          int adst = in->dst;
          int arg = in->a;
          nyir_op_t oop = op->op;
          f->data[i] = (nyir_inst_t){.op = NYIR_CONST_I64,
                                        .dst = cdst,
                                        .imm = imm,
                                        .range = {.has_min = true,
                                                  .has_max = true,
                                                  .min = imm,
                                                  .max = imm}};
          f->data[i + 1] = (nyir_inst_t){
              .op = oop, .dst = adst, .a = arg, .b = cdst};
          i++;
          continue;
        }
      }
    }

    /*
     * Generic two-parameter binary operation callee inlining:
     * trace return value back to an operation on parameters (add, mul, cmp, etc.)
     */
    int b_op = -1, b_p0 = -1, b_p1 = -1;
    nyir_cmp_t b_cmp = NYIR_CMP_EQ;
    int ret_val = -1;
    for (size_t k = 0; k < callee->len; ++k) {
      if (callee->data[k].op == NYIR_RET) {
        ret_val = callee->data[k].a;
        break;
      }
    }
    if (ret_val >= 0 && in->dst >= 0) {
      for (int depth = 0; depth < 16; ++depth) {
        int def_i = -1;
        for (size_t k = 0; k < callee->len; ++k) {
          if (callee->data[k].dst == ret_val) { def_i = (int)k; break; }
        }
        if (def_i < 0) break;
        const nyir_inst_t *def = &callee->data[def_i];
        if (def->op == NYIR_COPY && def->a >= 0) {
          ret_val = def->a;
        } else if (nyir_is_i64_binop(def->op) || def->op == NYIR_ADD_F64 ||
                   def->op == NYIR_SUB_F64 || def->op == NYIR_MUL_F64 ||
                   def->op == NYIR_DIV_F64 || def->op == NYIR_CMP_I64 ||
                   def->op == NYIR_CMP_F64) {
          int p0 = def->a, p1 = def->b;
          int param0 = (p0 >= 0 && p0 < (int)callee->param_count) ? p0 : -1;
          int param1 = (p1 >= 0 && p1 < (int)callee->param_count) ? p1 : -1;
          for (size_t k = 0; k < callee->len; ++k) {
            const nyir_inst_t *c = &callee->data[k];
            if (c->dst == p0 && c->op == NYIR_LOAD_LOCAL && c->imm >= 0 && c->imm < (int64_t)callee->param_count) param0 = (int)c->imm;
            if (c->dst == p1 && c->op == NYIR_LOAD_LOCAL && c->imm >= 0 && c->imm < (int64_t)callee->param_count) param1 = (int)c->imm;
            if (c->dst == p0 && c->op == NYIR_COPY && c->a >= 0 && c->a < (int)callee->param_count) param0 = c->a;
            if (c->dst == p1 && c->op == NYIR_COPY && c->a >= 0 && c->a < (int)callee->param_count) param1 = c->a;
          }
          if (param0 >= 0 && param1 >= 0) {
            b_op = def->op; b_p0 = param0; b_p1 = param1; b_cmp = def->cmp;
          }
          break;
        } else break;
      }
    }
    if (b_op >= 0 && b_p0 >= 0 && b_p1 >= 0 && (in->imm >= 2 || (in->a >= 0 && in->b >= 0))) {
      int call_args[6] = {in->a, in->b, in->c, in->d, in->e, in->f};
      int arg0 = b_p0 < 6 ? call_args[b_p0] : -1;
      int arg1 = b_p1 < 6 ? call_args[b_p1] : -1;
      if (arg0 >= 0 && arg1 >= 0) {
        *in = (nyir_inst_t){.op = (nyir_op_t)b_op,
                              .dst = in->dst,
                              .a = arg0,
                              .b = arg1,
                              .cmp = b_cmp};
        continue;
      }
    }

    /*
     * Three-inst with const in middle: load0; const; binop; ret (len 4).
     */
    if (callee->len == 4 && in->a >= 0 && in->dst >= 0 &&
        callee->data[0].op == NYIR_LOAD_LOCAL && callee->data[0].imm == 0 &&
        callee->data[1].op == NYIR_CONST_I64 &&
        nyir_is_i64_binop(callee->data[2].op) &&
        callee->data[2].a == callee->data[0].dst &&
        callee->data[2].b == callee->data[1].dst &&
        callee->data[3].op == NYIR_RET &&
        callee->data[3].a == callee->data[2].dst) {
      if (!nyir_ensure_insert(f, i, 1))
        return false;
      int cdst = f->next_value++;
      int adst = in->dst;
      int arg = in->a;
      int64_t imm = callee->data[1].imm;
      nyir_op_t oop = callee->data[2].op;
      f->data[i] = (nyir_inst_t){.op = NYIR_CONST_I64,
                                    .dst = cdst,
                                    .imm = imm,
                                    .range = {.has_min = true,
                                              .has_max = true,
                                              .min = imm,
                                              .max = imm}};
      f->data[i + 1] =
          (nyir_inst_t){.op = oop, .dst = adst, .a = arg, .b = cdst};
      i++;
      continue;
    }

    /*
     * General pure body splice (≤12 insts): map load.local p → arg, remap
     * temps, replace CALL with inlined stream ending in COPY to call dst.
     */
    if (callee->len <= 14 && callee->len >= 2 && in->dst >= 0) {
      int args[6] = {in->a, in->b, in->c, in->d, in->e, in->f};
      int arity = (int)in->imm;
      if (arity < 0) arity = 0;
      if (arity > 6) arity = 6;
      bool ok_s = true;
      int max_loc = -1;
      for (size_t k = 0; k < callee->len; ++k) {
        const nyir_inst_t *c = &callee->data[k];
        if (c->op == NYIR_NOP || c->op == NYIR_RET || c->op == NYIR_LABEL) continue;
        if (c->op == NYIR_LOAD_LOCAL) {
          if (c->imm > max_loc) max_loc = (int)c->imm;
          continue;
        }
        if (c->op == NYIR_STORE_LOCAL) { ok_s = false; break; }
        if (c->op == NYIR_CONST_I64 || c->op == NYIR_CONST_F64 ||
            c->op == NYIR_COPY || nyir_is_i64_binop(c->op) ||
            c->op == NYIR_ADD_F64 || c->op == NYIR_SUB_F64 ||
            c->op == NYIR_MUL_F64 || c->op == NYIR_DIV_F64 ||
            c->op == NYIR_CMP_I64 || c->op == NYIR_CMP_F64)
          continue;
        ok_s = false;
        break;
      }
      if (ok_s && max_loc < arity) {
        size_t body_n = 0;
        for (size_t k = 0; k < callee->len; ++k)
          if (callee->data[k].op != NYIR_RET && callee->data[k].op != NYIR_NOP &&
              callee->data[k].op != NYIR_LABEL)
            body_n++;
        if (body_n > 0 && body_n <= 12) {
          int *vmap = NULL;
          if (callee->next_value > 0) {
            vmap = calloc((size_t)callee->next_value, sizeof(int));
            if (!vmap) return false;
            for (int v = 0; v < callee->next_value; ++v) vmap[v] = -1;
          }
          if (!nyir_ensure_insert(f, i, body_n)) {
            free(vmap);
            return false;
          }
          size_t w = i;
          int call_dst = in->dst;
          for (size_t k = 0; k < callee->len; ++k) {
            const nyir_inst_t *c = &callee->data[k];
            if (c->op == NYIR_NOP || c->op == NYIR_LABEL) continue;
            if (c->op == NYIR_RET) {
              int src = -1;
              if (c->a >= 0 && vmap && c->a < callee->next_value && vmap[c->a] >= 0)
                src = vmap[c->a];
              else if (c->a >= 0 && c->a < arity)
                src = args[c->a];
              if (src >= 0)
                f->data[w++] = (nyir_inst_t){.op = NYIR_COPY, .dst = call_dst, .a = src, .b = -1};
              continue;
            }
            if (c->op == NYIR_LOAD_LOCAL && c->imm >= 0 && (int)c->imm < arity) {
              int nd = f->next_value++;
              if (vmap && c->dst >= 0 && c->dst < callee->next_value)
                vmap[c->dst] = nd;
              f->data[w++] = (nyir_inst_t){.op = NYIR_COPY, .dst = nd, .a = args[c->imm], .b = -1};
              continue;
            }
            nyir_inst_t ni = *c;
            if (ni.dst >= 0) {
              int nd = f->next_value++;
              if (vmap && ni.dst < callee->next_value)
                vmap[ni.dst] = nd;
              ni.dst = nd;
            }
            if (ni.a >= 0 && vmap && ni.a < callee->next_value && vmap[ni.a] >= 0)
              ni.a = vmap[ni.a];
            if (ni.b >= 0 && vmap && ni.b < callee->next_value && vmap[ni.b] >= 0)
              ni.b = vmap[ni.b];
            f->data[w++] = ni;
          }
          for (size_t z = w; z <= i + body_n && z < f->len; ++z)
            f->data[z] = (nyir_inst_t){.op = NYIR_NOP, .dst = -1, .a = -1, .b = -1};
          free(vmap);
          i = w > 0 ? w - 1 : i;
          continue;
        }
      }
    }
    (void)ret_src;
  }
  return true;
}

/*
 * Opt-in inliner remarks.  Keep explanation work off the optimizer fast path
 * unless explicitly requested so normal compilation does not allocate or
 * format diagnostics.
 */
static bool nyir_inline_trace_enabled(void) {
  static int initialized = 0;
  static bool enabled = false;
  if (!initialized) {
    const char *v = getenv("NY_TRACE_INLINE");
    enabled = v && v[0] && strcmp(v, "0") != 0;
    initialized = 1;
  }
  return enabled;
}

static void nyir_inline_trace(const nyir_func_t *caller, size_t call_idx,
                              const char *callee, const char *status,
                              const char *reason) {
  if (!nyir_inline_trace_enabled())
    return;
  const nyir_inst_t *call = caller && call_idx < caller->len
                                ? &caller->data[call_idx]
                                : NULL;
  const char *file = call && call->debug.file ? call->debug.file : "<unknown>";
  unsigned line = call ? call->debug.line : 0;
  unsigned column = call ? call->debug.column : 0;
  fprintf(stderr,
          "nyir inline: %s pc=%zu callee=%s at %s:%u:%u reason=%s\n",
          status ? status : "remark", call_idx,
          callee ? callee : "<unknown>", file, line, column,
          reason ? reason : "none");
}

static bool nyir_inline_value_is_const(const nyir_func_t *f, size_t before,
                                       int value) {
  if (!f || value < 0)
    return false;
  if (before > f->len)
    before = f->len;
  for (size_t i = before; i-- > 0;) {
    const nyir_inst_t *in = &f->data[i];
    if (in->dst != value)
      continue;
    return in->op == NYIR_CONST_I64 || in->op == NYIR_CONST_F64 ||
           in->op == NYIR_CONST_F32;
  }
  return false;
}


/*
 * Cheap structural loop-depth estimate for profitability.  Canonical NYIR
 * loops carry a backward branch to an earlier label; count the intervals that
 * contain the call.  This is deliberately only a cost-model hint: legality
 * and correctness never depend on the estimate, and deeply nested credit is
 * capped by the caller below.
 */
static size_t nyir_inline_static_loop_depth(const nyir_func_t *f,
                                            size_t call_idx) {
  if (!f || call_idx >= f->len)
    return 0;
  size_t depth = 0;
  for (size_t i = call_idx + 1; i < f->len; ++i) {
    const nyir_inst_t *term = &f->data[i];
    if (term->op != NYIR_BR && term->op != NYIR_BR_IF)
      continue;
    size_t target = SIZE_MAX;
    for (size_t j = 0; j <= call_idx; ++j) {
      if (f->data[j].op == NYIR_LABEL && f->data[j].imm == term->imm) {
        target = j;
        break;
      }
    }
    if (target != SIZE_MAX && target <= call_idx) {
      ++depth;
      if (depth == 3)
        break;
    }
  }
  return depth;
}

/*
 * Cost model for inlining.  Profile heat raises the size budget, constants and
 * optimization-exposing operations add a bounded benefit credit, while a high
 * temporary-value count approximates post-inline register-pressure growth.
 */
static bool nyir_should_inline_general(const nyir_func_t *caller,
                                       const nyir_inst_t *call,
                                       const nyir_func_t *callee,
                                       size_t call_idx) {
  const char *symbol = call ? call->symbol : NULL;
  if (!caller || !call || !callee || callee == caller || callee->len == 0 ||
      call->imm < 0 || call->imm > NYIR_CALL_MAX_ARGS ||
      (call->flags & NYIR_INST_F_SRET)) {
    nyir_inline_trace(caller, call_idx, symbol, "reject",
                      "unsupported call shape or recursive/sret callee");
    return false;
  }

  uint64_t block_heat = ny_native_profile_block_hot(call_idx);
  uint64_t loop_heat = ny_native_profile_loop_hot(call_idx);
  uint64_t edge_heat = ny_native_profile_edge_hot(call_idx, call_idx + 1);
  bool hot = (ny_native_profile_steps() > 10000 && edge_heat > 100) ||
             (ny_native_profile_steps() > 1000 && block_heat > 50) ||
             loop_heat >= 32;
  size_t max_callee_size = hot ? 128 : 64;

  int args[NYIR_CALL_MAX_ARGS];
  int arity = 0;
  if (!nyir_call_args(call, caller->next_value, args, NYIR_CALL_MAX_ARGS,
                      &arity, NULL, 0)) {
    nyir_inline_trace(caller, call_idx, symbol, "reject",
                      "malformed call argument vector");
    return false;
  }

  size_t constant_args = 0;
  for (int a = 0; a < arity; ++a)
    if (nyir_inline_value_is_const(caller, call_idx, args[a]))
      ++constant_args;
  size_t static_loop_depth = nyir_inline_static_loop_depth(caller, call_idx);

  /*
   * A call in a statically-recognizable loop pays its overhead every
   * iteration even when runtime profile data is unavailable (the normal
   * ahead-of-time / warm-no-exec benchmark case).  The old 64-instruction
   * ceiling therefore rejected medium-sized loop helpers before later
   * scalarization/BCE/vectorization could see through the call boundary.
   * Keep straight-line cold callers conservative, but give repeated static
   * loop calls the same first-tier budget as a hot call, and a little more
   * for nested loops where the amortized benefit is larger.
   */
  if (static_loop_depth >= 2 && max_callee_size < 192)
    max_callee_size = 192;
  else if (static_loop_depth >= 1 && max_callee_size < 128)
    max_callee_size = 128;

  size_t optimization_credit = constant_args * 6 + static_loop_depth * 8;
  for (size_t k = 0; k < callee->len; ++k) {
    const nyir_inst_t *in = &callee->data[k];
    /*
     * Immutable parameter loads are substituted with caller SSA arguments.
     * Mutable/address-taken formals are legal too: the clone path materializes
     * only those bindings into fresh caller-local slots before entering the
     * cloned body.
     */
    if ((in->op == NYIR_LOAD_LOCAL || in->op == NYIR_STORE_LOCAL ||
         in->op == NYIR_ADDR_LOCAL) &&
        (in->imm < 0 || in->imm >= INT_MAX)) {
      nyir_inline_trace(caller, call_idx, symbol, "reject",
                        "callee has an invalid local-slot reference");
      return false;
    }
    if (in->op == NYIR_ALLOCA)
      optimization_credit += 4;
    else if (in->op == NYIR_BOUNDS_CHECK)
      optimization_credit += 3;
  }
  if (optimization_credit > 32)
    optimization_credit = 32;

  size_t pressure_penalty = 0;
  if (callee->next_value > 32) {
    pressure_penalty = (size_t)(callee->next_value - 32) / 2;
    /*
     * Register pressure still matters, but an unbounded synthetic penalty
     * used to defeat otherwise-profitable repeated-call inlining.
     */
    if (static_loop_depth && pressure_penalty > 32)
      pressure_penalty = 32;
  }
  size_t effective_size = callee->len;
  if (pressure_penalty > SIZE_MAX - effective_size)
    effective_size = SIZE_MAX;
  else
    effective_size += pressure_penalty;
  size_t budget = max_callee_size;
  if (optimization_credit <= SIZE_MAX - budget)
    budget += optimization_credit;

  if (effective_size > budget) {
    nyir_inline_trace(caller, call_idx, symbol, "reject",
                      "code-size/register-pressure cost exceeds benefit budget");
    return false;
  }

  nyir_inline_trace(caller, call_idx, symbol, "accept",
                    hot ? "hot call is legal and profitable"
                        : (static_loop_depth
                               ? "loop nesting raises repeated-call benefit"
                               : (constant_args
                                      ? "constant exposure offsets inline cost"
                                      : "legal callee fits inline budget")));
  return true;
}

static bool nyir_inline_push(nyir_func_t *f, nyir_inst_t in) {
  /*
   * Synthesized control-flow/local instructions must carry the same canonical
   * effect mask as instructions emitted through nyir_emit().  Recompute here
   * so clone/splice helpers cannot accidentally leave stale metadata behind.
   */
  in.effects = nyir_inst_effects(&in);
  if (f->len == f->cap) {
    size_t cap = f->cap ? f->cap * 2 : 32;
    nyir_inst_t *data = ny_realloc_array(f->data, cap, sizeof(*data));
    if (!data)
      return false;
    f->data = data;
    f->cap = cap;
  }
  f->data[f->len++] = in;
  return true;
}

/*
 * Move instruction-owned arrays from a deep-cloned callee into `body`.
 */
static bool nyir_inline_take(nyir_func_t *body, nyir_inst_t *in) {
  if (!nyir_inline_push(body, *in))
    return false;
  in->extra_args = NULL;
  in->extra_args_len = 0;
  in->arg_sizes = NULL;
  in->phi_incoming = NULL;
  in->phi_incoming_len = 0;
  return true;
}

static bool nyir_inline_next_label(const nyir_func_t *f, int64_t *out) {
  int64_t next = 0;
  for (size_t i = 0; i < f->len; ++i) {
    if (f->data[i].op != NYIR_LABEL || f->data[i].imm < next)
      continue;
    if (f->data[i].imm == INT64_MAX)
      return false;
    next = f->data[i].imm + 1;
  }
  *out = next;
  return true;
}

static int nyir_inline_block_for_label(const nyir_cfg_t *cfg, int64_t label) {
  for (size_t b = 0; b < cfg->block_count; ++b)
    if (cfg->block_label[b] == label)
      return (int)b;
  return -1;
}

static int nyir_inline_value(const int *vmap, int n, int value) {
  return value < 0 ? value : (!vmap || value >= n ? -1 : vmap[value]);
}

static int nyir_inline_local_count(const nyir_func_t *f) {
  int max_slot = -1;
  if (!f)
    return 0;
  for (size_t i = 0; i < f->len; ++i) {
    const nyir_inst_t *in = &f->data[i];
    if ((in->op == NYIR_LOAD_LOCAL || in->op == NYIR_STORE_LOCAL ||
         in->op == NYIR_ADDR_LOCAL) &&
        in->imm >= 0 && in->imm < INT_MAX && in->imm > max_slot)
      max_slot = (int)in->imm;
  }
  return max_slot + 1;
}

/*
 * Clone a callee with fresh SSA values and labels. PHIs are remapped from a
 * complete value map (including backedges), all RETs converge at a fresh
 * continuation PHI, and nested calls retain deep-copied owned metadata.
 */
static size_t nyir_inline_clone_body(nyir_func_t *caller, size_t call_idx,
                                     const nyir_func_t *callee,
                                     const int *arg_values, int arity,
                                     int call_dst, int *out_ret_dst) {
  if (!caller || !callee || callee->len == 0 || call_idx >= caller->len ||
      arity < 0 || arity > NYIR_CALL_MAX_ARGS)
    return 0;

  nyir_func_t clone = {0}, body = {0};
  nyir_cfg_t ccfg = {0}, fcfg = {0};
  int *vmap = NULL, *ret_values = NULL;
  int64_t *labels = NULL, *ret_labels = NULL;
  bool materialized_formal[NYIR_CALL_MAX_ARGS] = {false};
  int formal_local[NYIR_CALL_MAX_ARGS];
  int caller_local_count = 0, callee_local_count = 0, local_base = 0;
  int materialized_count = 0;
  int next_value = caller->next_value;
  int64_t next_label = 0, continuation = -1;
  size_t ret_count = 0, ret_at = 0, committed = 0;
  size_t call_block = 0;
  int64_t old_pred_label = -1;
  bool ok = false;

  if (!nyir_func_clone(callee, &clone) ||
      !nyir_cfg_build_topology(&clone, &fcfg) || fcfg.block_count == 0 ||
      !nyir_cfg_build_topology(caller, &ccfg))
    goto done;
  caller_local_count = nyir_inline_local_count(caller);
  callee_local_count = nyir_inline_local_count(&clone);
  for (int p = 0; p < NYIR_CALL_MAX_ARGS; ++p)
    formal_local[p] = -1;
  for (size_t i = 0; i < clone.len; ++i) {
    const nyir_inst_t *in = &clone.data[i];
    if ((in->op != NYIR_STORE_LOCAL && in->op != NYIR_ADDR_LOCAL) ||
        in->imm < 0 || in->imm >= arity)
      continue;
    if (!materialized_formal[in->imm]) {
      materialized_formal[in->imm] = true;
      ++materialized_count;
    }
  }
  int private_count = callee_local_count > arity ? callee_local_count - arity : 0;
  if (materialized_count > INT_MAX - private_count ||
      caller_local_count > INT_MAX - (materialized_count + private_count))
    goto done;
  local_base = caller_local_count;
  int next_formal_local = local_base;
  for (int p = 0; p < arity; ++p)
    if (materialized_formal[p])
      formal_local[p] = next_formal_local++;
  call_block = ccfg.inst_block[call_idx];
  old_pred_label = ccfg.block_label[call_block];

  /*
   * Inlining materializes the implicit invocation edge. An entry PHI has no
   * encoded value for that edge, so keep that unusual CFG shape out.
   */
  {
    size_t at = fcfg.block_start[0];
    if (at < fcfg.block_end[0] && clone.data[at].op == NYIR_LABEL)
      ++at;
    while (at < fcfg.block_end[0] && clone.data[at].op == NYIR_NOP)
      ++at;
    if (at < fcfg.block_end[0] && clone.data[at].op == NYIR_PHI)
      goto done;
  }

  /*
   * Do not let the cloned function physically fall through into caller code.
   */
  {
    size_t end = clone.len;
    while (end > 0 && (clone.data[end - 1].op == NYIR_NOP ||
                       clone.data[end - 1].op == NYIR_LABEL))
      --end;
    if (end == 0 || (clone.data[end - 1].op != NYIR_RET &&
                     clone.data[end - 1].op != NYIR_BR))
      goto done;
  }

  if (!nyir_inline_next_label(caller, &next_label) ||
      fcfg.block_count > (size_t)(INT64_MAX - next_label))
    goto done;
  labels = ny_malloc_array(fcfg.block_count, sizeof(*labels));
  if (!labels)
    goto done;
  for (size_t b = 0; b < fcfg.block_count; ++b)
    labels[b] = next_label++;
  if (next_label == INT64_MAX)
    goto done;
  continuation = next_label++;

  if (clone.next_value > 0) {
    vmap = ny_malloc_array((size_t)clone.next_value, sizeof(*vmap));
    if (!vmap)
      goto done;
    for (int v = 0; v < clone.next_value; ++v)
      vmap[v] = -1;
  }

  /*
   * Allocate every definition up front so PHI backedge values remap even when
   * their defining instruction appears later in linear order.
   */
  for (size_t i = 0; i < clone.len; ++i) {
    nyir_inst_t *in = &clone.data[i];
    if (in->dst < 0)
      continue;
    if (!vmap || in->dst >= clone.next_value)
      goto done;
    if (in->op == NYIR_LOAD_LOCAL && in->imm >= 0 && in->imm < arity &&
        !materialized_formal[in->imm]) {
      int actual = arg_values ? arg_values[in->imm] : -1;
      if (actual < 0 || actual >= caller->next_value)
        goto done;
      vmap[in->dst] = actual;
    } else {
      if (next_value == INT_MAX)
        goto done;
      vmap[in->dst] = next_value++;
    }
  }

  for (size_t i = 0; i < clone.len; ++i)
    if (clone.data[i].op == NYIR_RET)
      ++ret_count;
  if (call_dst >= 0 && ret_count == 0)
    goto done;
  if (ret_count) {
    ret_labels = ny_malloc_array(ret_count, sizeof(*ret_labels));
    if (!ret_labels)
      goto done;
    if (call_dst >= 0) {
      ret_values = ny_malloc_array(ret_count, sizeof(*ret_values));
      if (!ret_values)
        goto done;
    }
  }

  for (size_t b = 0; b < fcfg.block_count; ++b) {
    if (!nyir_inline_push(&body, (nyir_inst_t){
            .op = NYIR_LABEL, .dst = -1, .a = -1, .b = -1, .c = -1,
            .d = -1, .e = -1, .f = -1, .imm = labels[b]}))
      goto done;

    if (b == 0) {
      for (int p = 0; p < arity; ++p) {
        if (!materialized_formal[p])
          continue;
        int actual = arg_values ? arg_values[p] : -1;
        if (actual < 0 || actual >= caller->next_value || formal_local[p] < 0)
          goto done;
        if (!nyir_inline_push(&body, (nyir_inst_t){
                .op = NYIR_STORE_LOCAL, .dst = -1, .a = actual, .b = -1,
                .c = -1, .d = -1, .e = -1, .f = -1,
                .imm = formal_local[p], .effects = NYIR_EFFECT_WRITE_LOCAL}))
          goto done;
      }
    }

    for (size_t i = fcfg.block_start[b]; i < fcfg.block_end[b]; ++i) {
      nyir_inst_t *in = &clone.data[i];
      if (in->op == NYIR_NOP || in->op == NYIR_LABEL ||
          (in->op == NYIR_LOAD_LOCAL && in->imm >= 0 && in->imm < arity &&
           !materialized_formal[in->imm]))
        continue;

      if (in->op == NYIR_RET) {
        ret_labels[ret_at] = labels[b];
        if (call_dst >= 0) {
          int value = nyir_inline_value(vmap, clone.next_value, in->a);
          if (value < 0 || next_value == INT_MAX)
            goto done;
          ret_values[ret_at] = next_value;
          if (!nyir_inline_push(&body, (nyir_inst_t){
                  .op = NYIR_COPY, .dst = next_value++, .a = value, .b = -1,
                  .c = -1, .d = -1, .e = -1, .f = -1}))
            goto done;
        }
        ++ret_at;
        if (!nyir_inline_push(&body, (nyir_inst_t){
                .op = NYIR_BR, .dst = -1, .a = -1, .b = -1, .c = -1,
                .d = -1, .e = -1, .f = -1, .imm = continuation}))
          goto done;
        continue;
      }

      if (in->op == NYIR_BR || in->op == NYIR_BR_IF) {
        int target = nyir_inline_block_for_label(&fcfg, in->imm);
        if (target < 0)
          goto done;
        in->imm = labels[target];
        if (in->op == NYIR_BR_IF &&
            (in->a = nyir_inline_value(vmap, clone.next_value, in->a)) < 0)
          goto done;
      } else if (in->op == NYIR_PHI) {
        in->dst = nyir_inline_value(vmap, clone.next_value, in->dst);
        if (in->dst < 0)
          goto done;
        for (size_t p = 0; p < in->phi_incoming_len; ++p) {
          int pred = nyir_inline_block_for_label(
              &fcfg, in->phi_incoming[p].predecessor_label);
          int value = nyir_inline_value(vmap, clone.next_value,
                                        in->phi_incoming[p].value);
          if (pred < 0 || value < 0)
            goto done;
          in->phi_incoming[p].predecessor_label = labels[pred];
          in->phi_incoming[p].value = value;
        }
      } else {
        if ((in->op == NYIR_LOAD_LOCAL || in->op == NYIR_STORE_LOCAL ||
             in->op == NYIR_ADDR_LOCAL) && in->imm >= 0) {
          if (in->imm < arity) {
            if (!materialized_formal[in->imm] || formal_local[in->imm] < 0)
              goto done;
            in->imm = formal_local[in->imm];
          } else {
            int64_t offset = in->imm - arity;
            int64_t mapped = (int64_t)local_base + materialized_count + offset;
            if (mapped < 0 || mapped >= INT_MAX)
              goto done;
            in->imm = mapped;
          }
        }
        if (in->dst >= 0 &&
            (in->dst = nyir_inline_value(vmap, clone.next_value, in->dst)) < 0)
          goto done;
        int *ops[] = {&in->a, &in->b, &in->c, &in->d, &in->e, &in->f};
        for (size_t p = 0; p < sizeof(ops) / sizeof(ops[0]); ++p)
          if (*ops[p] >= 0 &&
              (*ops[p] = nyir_inline_value(vmap, clone.next_value, *ops[p])) < 0)
            goto done;
        for (size_t p = 0; p < in->extra_args_len; ++p)
          if ((in->extra_args[p] = nyir_inline_value(
                   vmap, clone.next_value, in->extra_args[p])) < 0)
            goto done;
      }

      if (!nyir_inline_take(&body, in))
        goto done;
    }
  }

  if (!nyir_inline_push(&body, (nyir_inst_t){
          .op = NYIR_LABEL, .dst = -1, .a = -1, .b = -1, .c = -1,
          .d = -1, .e = -1, .f = -1, .imm = continuation}))
    goto done;
  if (call_dst >= 0) {
    nyir_phi_incoming_t *incoming = ny_malloc_array(ret_count, sizeof(*incoming));
    if (!incoming)
      goto done;
    for (size_t r = 0; r < ret_count; ++r)
      incoming[r] = (nyir_phi_incoming_t){ret_labels[r], ret_values[r]};
    if (!nyir_inline_push(&body, (nyir_inst_t){
            .op = NYIR_PHI, .dst = call_dst, .a = -1, .b = -1, .c = -1,
            .d = -1, .e = -1, .f = -1, .phi_incoming = incoming,
            .phi_incoming_len = ret_count})) {
      free(incoming);
      goto done;
    }
  }

  if (body.len == 0 || body.len > SIZE_MAX - (caller->len - 1))
    goto done;
  size_t new_len = caller->len - 1 + body.len;
  if (new_len > caller->cap) {
    nyir_inst_t *data = ny_realloc_array(caller->data, new_len, sizeof(*data));
    if (!data)
      goto done;
    caller->data = data;
    caller->cap = new_len;
  }
  if (clone.owned_symbols_len > SIZE_MAX - caller->owned_symbols_len)
    goto done;
  size_t owned_need = caller->owned_symbols_len + clone.owned_symbols_len;
  if (owned_need > caller->owned_symbols_cap) {
    char **owned = ny_realloc_array(caller->owned_symbols, owned_need, sizeof(*owned));
    if (!owned)
      goto done;
    caller->owned_symbols = owned;
    caller->owned_symbols_cap = owned_need;
  }

  nyir_inst_discard(&caller->data[call_idx]);
  size_t tail = caller->len - call_idx - 1;
  memmove(&caller->data[call_idx + body.len], &caller->data[call_idx + 1],
          tail * sizeof(*caller->data));
  memcpy(&caller->data[call_idx], body.data, body.len * sizeof(*caller->data));
  caller->len = new_len;
  caller->next_value = next_value;
  for (size_t i = 0; i < clone.owned_symbols_len; ++i)
    caller->owned_symbols[caller->owned_symbols_len++] = clone.owned_symbols[i];

  /*
   * The original post-call tail is now in the continuation block. Update PHI
   * edges in each old successor to name that block instead of the split one.
   */
  size_t delta = body.len - 1;
  for (size_t e = ccfg.succ_offsets[call_block];
       e < ccfg.succ_offsets[call_block + 1]; ++e) {
    size_t at = ccfg.block_start[ccfg.succ_blocks[e]];
    if (at > call_idx)
      at += delta;
    if (at < caller->len && caller->data[at].op == NYIR_LABEL)
      ++at;
    while (at < caller->len &&
           (caller->data[at].op == NYIR_NOP ||
            caller->data[at].op == NYIR_PHI)) {
      if (caller->data[at].op == NYIR_PHI)
        for (size_t p = 0;
             p < caller->data[at].phi_incoming_len; ++p)
          if (caller->data[at].phi_incoming[p].predecessor_label ==
              old_pred_label)
            caller->data[at].phi_incoming[p].predecessor_label =
                continuation;
      ++at;
    }
  }

  committed = body.len;
  free(body.data);
  body.data = NULL;
  body.len = body.cap = 0;
  free(clone.owned_symbols);
  clone.owned_symbols = NULL;
  clone.owned_symbols_len = clone.owned_symbols_cap = 0;
  if (out_ret_dst)
    *out_ret_dst = call_dst;
  ok = true;

done:
  nyir_func_free(&body);
  nyir_func_free(&clone);
  nyir_cfg_free(&ccfg);
  nyir_cfg_free(&fcfg);
  free(vmap);
  free(labels);
  free(ret_values);
  free(ret_labels);
  return ok ? committed : 0;
}

/*
 * General inliner: inline profitable calls using the above machinery.
 */
bool nyir_inline_general(nyir_func_t *f) {
  if (!f || nyir_inline_callee_count == 0)
    return true;

  size_t max_rounds = nyir_inline_callee_count + 1u;
  if (max_rounds > 256u)
    max_rounds = 256u;
  for (size_t iter = 0; iter < max_rounds; ++iter) {
    bool changed = false;
    for (size_t i = 0; i < f->len; ++i) {      nyir_inst_t *call = &f->data[i];
      if (call->op != NYIR_CALL || !call->symbol)
        continue;

      const nyir_func_t *callee = nyir_find_inline_callee(call->symbol);
      if (!callee) {
        nyir_inline_trace(f, i, call->symbol, "reject",
                          "no registered monomorphic NYIR callee body");
        continue;
      }
      if (!nyir_should_inline_general(f, call, callee, i))
        continue;
      int args[NYIR_CALL_MAX_ARGS];
      int arity = 0;
      if (!nyir_call_args(call, f->next_value, args, NYIR_CALL_MAX_ARGS, &arity, NULL, 0))
        continue;
      int call_dst = call->dst;
      int ret_dst = -1;
      size_t inserted =
          nyir_inline_clone_body(f, i, callee, args, arity, call_dst, &ret_dst);
      if (inserted > 0) {
        changed = true;
        i += inserted - 1;
      } else {
        nyir_inline_trace(f, i, call->symbol, "reject",
                          "callee clone/CFG splice legality check failed");
      }
    }
    if (!changed)
      break;
  }
  return true;
}

/*
 * Loop unrolling is implemented here with the other advanced NYIR transforms;
 * the pass keeps its local CFG and profile helpers next to its implementation.
 */
bool nyir_loop_unroll(nyir_func_t *f) {
  /*
   * Enhanced loop unroll:
   * - Full unroll for tiny counted loops (trip 2-16)
   * - Partial unroll with remainder handling for larger loops
   * - Configurable unroll factor based on loop heat/profile
   * - Remainder handling via cleanup loop
   * - Supports: CMP_I64 LT with constant trip, pure scalar bodies
   */
  if (!f || f->len < 8)
    return true;

  for (size_t bi = 0; bi < f->len; ++bi) {
    if (f->data[bi].op != NYIR_BR_IF)
      continue;
    int cond = f->data[bi].a;
    int64_t body_lab = f->data[bi].imm;
    if (cond < 0)
      continue;
    /*
     * Find icmp.lt producing cond with const RHS trip.
     */
    int64_t trip = -1;
    int iv = -1;
    for (size_t j = 0; j < bi; ++j) {
      const nyir_inst_t *c = &f->data[j];
      if (c->op == NYIR_CMP_I64 && c->dst == cond && c->cmp == NYIR_CMP_LT) {
        iv = c->a;
        for (size_t k = 0; k < j; ++k)
          if (f->data[k].op == NYIR_CONST_I64 && f->data[k].dst == c->b) {
            trip = f->data[k].imm;
            break;
          }
        break;
      }
    }
    uint64_t loop_heat = ny_native_profile_loop_hot(bi);
    /*
     * Unroll factor based on loop heat (profile-guided).
     */
    int unroll_factor = loop_heat >= 100 ? 8 : loop_heat >= 16 ? 4 : 2;
    if (trip < 2 || iv < 0)
      continue;

    /*
     * Body label region.
     */
    size_t body_start = 0, body_end = 0;
    bool have = false;
    for (size_t j = 0; j < f->len; ++j) {
      if (f->data[j].op == NYIR_LABEL && f->data[j].imm == body_lab) {
        body_start = j + 1;
        for (size_t k = body_start; k < f->len; ++k) {
          if (f->data[k].op == NYIR_BR && f->data[k].imm != body_lab) {
            /*
             * unexpected
             */
          }
          if (f->data[k].op == NYIR_BR || f->data[k].op == NYIR_BR_IF ||
              f->data[k].op == NYIR_LABEL || f->data[k].op == NYIR_RET) {
            body_end = k;
            size_t body_limit = loop_heat >= 100 ? 96 : 64;
            have = body_end > body_start &&
                   (body_end - body_start) <= body_limit;
            break;
          }
        }
        break;
      }
    }
    if (!have)
      continue;
    /*
     * Latch must be br back to a header (label before the br_if).
     */
    if (f->data[body_end].op != NYIR_BR)
      continue;
    bool pure = true;
    for (size_t k = body_start; k < body_end; ++k) {
      nyir_op_t op = f->data[k].op;
      /*
       * Local loads/stores are safe across sequential copies: each copy keeps
       * the same local slot while the value map remaps its SSA operands.
       * Address-taken memory still needs alias-aware rematerialization.
       */
      if (op == NYIR_CALL || op == NYIR_PHI || op == NYIR_BR ||
          op == NYIR_BR_IF || op == NYIR_LABEL || op == NYIR_RET ||
          op == NYIR_ADDR_LOCAL || op == NYIR_ALLOCA) {
        pure = false;
        break;
      }
    }
    if (!pure)
      continue;

    size_t body_len = body_end - body_start;
    int64_t full_unroll_limit = loop_heat >= 100 ? 16 : 8;
    bool full_unroll = trip <= full_unroll_limit;

    if (full_unroll) {
      /*
       * Full unroll for tiny loops (original behavior).
       */
      size_t extra = (size_t)(trip - 1) * (body_end - body_start);
      if (extra == 0)
        continue;
      if (f->len + extra > f->cap) {
        size_t nc = f->cap ? f->cap * 2 : 64;
        while (nc < f->len + extra)
          nc *= 2;
        nyir_inst_t *nd = ny_realloc_array(f->data, nc, sizeof(*nd));
        if (!nd)
          return false;
        f->data = nd;
        f->cap = nc;
      }
      size_t insert_at = body_end;
      memmove(&f->data[insert_at + extra], &f->data[insert_at],
              (f->len - insert_at) * sizeof(nyir_inst_t));
      f->len += extra;
      for (int t = 1; t < (int)trip; ++t) {
        size_t base = insert_at + (size_t)(t - 1) * (body_end - body_start);
        int *vmap = NULL;
        size_t vmap_n = (size_t)(f->next_value + (int)(body_end - body_start) + 8);
        if (vmap_n < 8)
          vmap_n = 8;
        vmap = ny_malloc_array(vmap_n, sizeof(int));
        if (!vmap)
          return false;
        for (size_t m = 0; m < vmap_n; ++m)
          vmap[m] = -1;
        for (size_t k = 0; k < (size_t)(body_end - body_start); ++k) {
          nyir_inst_t in = f->data[body_start + k];
          int old_dst = in.dst;
          if (in.dst >= 0) {
            int nd = f->next_value++;
            if ((size_t)old_dst >= vmap_n) {
              size_t nn = (size_t)old_dst + 8;
              int *nv = ny_realloc_array(vmap, nn, sizeof(int));
              if (!nv) {
                free(vmap);
                return false;
              }
              for (size_t m = vmap_n; m < nn; ++m)
                nv[m] = -1;
              vmap = nv;
              vmap_n = nn;
            }
            vmap[old_dst] = nd;
            in.dst = nd;
          }
          if (in.a >= 0 && (size_t)in.a < vmap_n && vmap[in.a] >= 0)
            in.a = vmap[in.a];
          if (in.b >= 0 && (size_t)in.b < vmap_n && vmap[in.b] >= 0)
            in.b = vmap[in.b];
          if (in.c >= 0 && (size_t)in.c < vmap_n && vmap[in.c] >= 0)
            in.c = vmap[in.c];
          if (in.d >= 0 && (size_t)in.d < vmap_n && vmap[in.d] >= 0)
            in.d = vmap[in.d];
          f->data[base + k] = in;
        }
        free(vmap);
      }
      /*
       * After unrolling copies, force exit.
       */
      size_t br_if_at = bi;
      if (br_if_at >= body_end)
        br_if_at = bi;
      int64_t exit_lab = -1;
      for (size_t j = br_if_at + 1; j < f->len; ++j) {
        if (f->data[j].op == NYIR_NOP)
          continue;
        if (f->data[j].op == NYIR_BR) {
          exit_lab = f->data[j].imm;
          break;
        }
        if (f->data[j].op == NYIR_LABEL) {
          exit_lab = f->data[j].imm;
          break;
        }
        break;
      }
      if (exit_lab >= 0) {
        size_t latch = body_end + extra;
        if (latch < f->len && f->data[latch].op == NYIR_BR)
          f->data[latch].imm = exit_lab;
      }
      return true;
    } else {
      /*
       * Partial unroll with remainder loop.
       * Unroll factor = min(unroll_factor, trip).
       */
      int64_t uf = unroll_factor;
      if (uf > trip) uf = trip;
      int64_t full_chunks = trip / uf;
      int64_t remainder = trip % uf;
      if (full_chunks < 2)
        continue; /* Not worth partial unrolling. */

      size_t chunk_len = body_len * uf;
      size_t total_extra = (full_chunks - 1) * chunk_len;
      if (total_extra == 0)
        continue;
      if (f->len + total_extra > f->cap) {
        size_t nc = f->cap ? f->cap * 2 : 64;
        while (nc < f->len + total_extra)
          nc *= 2;
        nyir_inst_t *nd = ny_realloc_array(f->data, nc, sizeof(*nd));
        if (!nd)
          return false;
        f->data = nd;
        f->cap = nc;
      }

      /*
       * Unroll the first (full_chunks - 1) full chunks.
       */
      size_t insert_at = body_end;
      size_t accumulated_extra = 0;
      for (int64_t chunk = 1; chunk < full_chunks; ++chunk) {
        size_t base = insert_at + accumulated_extra;
        int *vmap = NULL;
        size_t vmap_n = (size_t)(f->next_value + (int)chunk_len + 8);
        if (vmap_n < 8)
          vmap_n = 8;
        vmap = ny_malloc_array(vmap_n, sizeof(int));
        if (!vmap)
          return false;
        for (size_t m = 0; m < vmap_n; ++m)
          vmap[m] = -1;
        for (size_t k = 0; k < chunk_len; ++k) {
          size_t src_idx = body_start + (k % body_len);
          nyir_inst_t in = f->data[src_idx];
          int old_dst = in.dst;
          if (in.dst >= 0) {
            int nd = f->next_value++;
            if ((size_t)old_dst >= vmap_n) {
              size_t nn = (size_t)old_dst + 8;
              int *nv = ny_realloc_array(vmap, nn, sizeof(int));
              if (!nv) {
                free(vmap);
                return false;
              }
              for (size_t m = vmap_n; m < nn; ++m)
                nv[m] = -1;
              vmap = nv;
              vmap_n = nn;
            }
            vmap[old_dst] = nd;
            in.dst = nd;
          }
          if (in.a >= 0 && (size_t)in.a < vmap_n && vmap[in.a] >= 0)
            in.a = vmap[in.a];
          if (in.b >= 0 && (size_t)in.b < vmap_n && vmap[in.b] >= 0)
            in.b = vmap[in.b];
          if (in.c >= 0 && (size_t)in.c < vmap_n && vmap[in.c] >= 0)
            in.c = vmap[in.c];
          if (in.d >= 0 && (size_t)in.d < vmap_n && vmap[in.d] >= 0)
            in.d = vmap[in.d];
          f->data[base + k] = in;
        }
        free(vmap);
        accumulated_extra += chunk_len;
      }
      f->len += total_extra;

      /*
       * Handle remainder: add a cleanup loop if remainder > 0.
       * Insert a new loop that runs the remainder iterations.
       */
      size_t rem_extra = 0;
      if (remainder > 0) {
        /*
         * Create a new loop for remainder iterations.
         * We'll insert it after the unrolled chunks, before the latch.
         */
        size_t insert_pos = body_end + total_extra;
        size_t remainder_body_len = body_len;
        size_t rem_extra = (remainder - 1) * remainder_body_len;
        if (rem_extra > 0 && f->len + rem_extra <= f->cap) {
          /*
           * Insert remainder loop body copies.
           */
          memmove(&f->data[insert_pos + rem_extra], &f->data[insert_pos],
                  (f->len - insert_pos) * sizeof(nyir_inst_t));
          f->len += rem_extra;
          for (int64_t t = 1; t < remainder; ++t) {
            int *vmap = ny_malloc_array((f->next_value + 16), sizeof(int));
            if (!vmap)
              return false;
            for (size_t m = 0; m < (size_t)(f->next_value + 16); ++m)
              vmap[m] = -1;
            for (size_t k = 0; k < remainder_body_len; ++k) {
              nyir_inst_t in = f->data[body_start + k];
              int old_dst = in.dst;
              if (in.dst >= 0) {
                int nd = f->next_value++;
                vmap[old_dst] = nd;
                in.dst = nd;
              }
              if (in.a >= 0 && vmap[in.a] >= 0) in.a = vmap[in.a];
              if (in.b >= 0 && vmap[in.b] >= 0) in.b = vmap[in.b];
              if (in.c >= 0 && vmap[in.c] >= 0) in.c = vmap[in.c];
              if (in.d >= 0 && vmap[in.d] >= 0) in.d = vmap[in.d];
              f->data[insert_pos + (t - 1) * remainder_body_len + k] = in;
            }
            free(vmap);
          }
        }
      }

      /*
       * Point latch at exit.
       */
      int64_t exit_lab = -1;
      for (size_t j = bi + 1; j < f->len; ++j) {
        if (f->data[j].op == NYIR_BR) {
          exit_lab = f->data[j].imm;
          break;
        }
        if (f->data[j].op == NYIR_LABEL) {
          exit_lab = f->data[j].imm;
          break;
        }
      }
      if (exit_lab >= 0) {
        size_t latch = body_end + total_extra + (remainder > 0 ? rem_extra : 0);
        if (latch < f->len && f->data[latch].op == NYIR_BR)
          f->data[latch].imm = exit_lab;
      }
      return true;
    }
  }
  return true;
}

bool nyir_algebraic_combine(nyir_func_t *f) {
  if (!f || f->next_value <= 0)
    return true;
  bool *known = calloc((size_t)f->next_value, sizeof(bool));
  int64_t *value = calloc((size_t)f->next_value, sizeof(int64_t));
  if (!known || !value) {
    free(known);
    free(value);
    return false;
  }
  for (size_t i = 0; i < f->len; ++i) {
    nyir_inst_t *in = &f->data[i];
    if (in->op == NYIR_CONST_I64 && in->dst >= 0 &&
        (size_t)in->dst < (size_t)f->next_value) {
      known[in->dst] = true;
      value[in->dst] = in->imm;
    }
    /*
     * (x + c1) + c2 → x + (c1+c2)
     */
    if (in->op == NYIR_ADD_I64 && in->a >= 0 && in->b >= 0 &&
        known[in->b]) {
      for (size_t j = 0; j < i; ++j) {
        nyir_inst_t *p = &f->data[j];
        if (p->op == NYIR_ADD_I64 && p->dst == in->a && p->b >= 0 &&
            known[p->b] && p->a >= 0) {
          int64_t sum = value[p->b] + value[in->b];
          int cdst = f->next_value++;
          if (f->len + 1 > f->cap) {
            size_t nc = f->cap ? f->cap * 2 : 64;
            nyir_inst_t *nd = ny_realloc_array(f->data, nc, sizeof(*nd));
            if (!nd) {
              free(known);
              free(value);
              return false;
            }
            f->data = nd;
            f->cap = nc;
            in = &f->data[i];
            p = &f->data[j];
          }
          memmove(&f->data[i + 1], &f->data[i],
                  (f->len - i) * sizeof(nyir_inst_t));
          f->len++;
          f->data[i] =
              (nyir_inst_t){.op = NYIR_CONST_I64,
                              .dst = cdst,
                              .imm = sum,
                              .range = {.has_min = true,
                                        .has_max = true,
                                        .min = sum,
                                        .max = sum}};
          f->data[i + 1] = (nyir_inst_t){
              .op = NYIR_ADD_I64, .dst = in->dst, .a = p->a, .b = cdst};
          bool *new_known = ny_realloc_array(known, (size_t)f->next_value, sizeof(bool));
          int64_t *new_value = ny_realloc_array(value, (size_t)f->next_value, sizeof(int64_t));
          if (!new_known || !new_value) {
            free(new_known ? new_known : known);
            free(new_value ? new_value : value);
            return false;
          }
          known = new_known;
          value = new_value;
          known[cdst] = true;
          value[cdst] = sum;
          i++;
          break;
        }
      }
    }
    /*
     * (x & m1) & m2 → x & (m1&m2)
     */
    if (in->op == NYIR_AND_I64 && in->a >= 0 && in->b >= 0 && known[in->b]) {
      for (size_t j = 0; j < i; ++j) {
        nyir_inst_t *p = &f->data[j];
        if (p->op == NYIR_AND_I64 && p->dst == in->a && p->b >= 0 &&
            known[p->b] && p->a >= 0) {
          int64_t m = value[p->b] & value[in->b];
          int cdst = f->next_value++;
          if (f->len + 1 > f->cap) {
            size_t nc = f->cap ? f->cap * 2 : 64;
            nyir_inst_t *nd = ny_realloc_array(f->data, nc, sizeof(*nd));
            if (!nd) {
              free(known);
              free(value);
              return false;
            }
            f->data = nd;
            f->cap = nc;
            in = &f->data[i];
            p = &f->data[j];
          }
          memmove(&f->data[i + 1], &f->data[i],
                  (f->len - i) * sizeof(nyir_inst_t));
          f->len++;
          f->data[i] =
              (nyir_inst_t){.op = NYIR_CONST_I64,
                              .dst = cdst,
                              .imm = m,
                              .range = {.has_min = true,
                                        .has_max = true,
                                        .min = m,
                                        .max = m}};
          f->data[i + 1] = (nyir_inst_t){
              .op = NYIR_AND_I64, .dst = in->dst, .a = p->a, .b = cdst};
          bool *new_known = ny_realloc_array(known, (size_t)f->next_value, sizeof(bool));
          int64_t *new_value = ny_realloc_array(value, (size_t)f->next_value, sizeof(int64_t));
          if (!new_known || !new_value) {
            free(new_known ? new_known : known);
            free(new_value ? new_value : value);
            return false;
          }
          known = new_known;
          value = new_value;
          known[cdst] = true;
          value[cdst] = m;
          i++;
          break;
        }
      }
    }
  }
  free(known);
  free(value);
  return true;
}

bool nyir_double_neg(nyir_func_t *f) {
  if (!f)
    return true;
  /*
   * ~(~x) / -(-x) via xor -1 twice or sub 0 patterns: x ^ -1 twice → copy x
   */
  for (size_t i = 0; i < f->len; ++i) {
    nyir_inst_t *in = &f->data[i];
    if (in->op != NYIR_XOR_I64 || in->a < 0 || in->b < 0)
      continue;
    /*
     * find if b is const -1 and a is also xor with -1
     */
    bool b_neg1 = false;
    for (size_t j = 0; j < i; ++j)
      if (f->data[j].op == NYIR_CONST_I64 && f->data[j].dst == in->b &&
          f->data[j].imm == -1)
        b_neg1 = true;
    if (!b_neg1)
      continue;
    for (size_t j = 0; j < i; ++j) {
      nyir_inst_t *p = &f->data[j];
      if (p->op == NYIR_XOR_I64 && p->dst == in->a && p->a >= 0 && p->b >= 0) {
        bool pb = false;
        for (size_t k = 0; k < j; ++k)
          if (f->data[k].op == NYIR_CONST_I64 && f->data[k].dst == p->b &&
              f->data[k].imm == -1)
            pb = true;
        if (pb) {
          *in = (nyir_inst_t){
              .op = NYIR_COPY, .dst = in->dst, .a = p->a, .b = -1};
          break;
        }
      }
    }
  }
  /*
   * sub 0, x → neg; sub 0, (sub 0, x) → copy x
   */
  for (size_t i = 0; i < f->len; ++i) {
    nyir_inst_t *in = &f->data[i];
    if (in->op != NYIR_SUB_I64 || in->a < 0 || in->b < 0)
      continue;
    bool a0 = false;
    for (size_t j = 0; j < i; ++j)
      if (f->data[j].op == NYIR_CONST_I64 && f->data[j].dst == in->a &&
          f->data[j].imm == 0)
        a0 = true;
    if (!a0)
      continue;
    for (size_t j = 0; j < i; ++j) {
      nyir_inst_t *p = &f->data[j];
      if (p->op == NYIR_SUB_I64 && p->dst == in->b && p->a >= 0 && p->b >= 0) {
        bool pa0 = false;
        for (size_t k = 0; k < j; ++k)
          if (f->data[k].op == NYIR_CONST_I64 && f->data[k].dst == p->a &&
              f->data[k].imm == 0)
            pa0 = true;
        if (pa0) {
          *in = (nyir_inst_t){
              .op = NYIR_COPY, .dst = in->dst, .a = p->b, .b = -1};
          break;
        }
      }
    }
  }
  return true;
}

bool nyir_reassoc_add(nyir_func_t *f) {
  /*
   * (a + b) + c with c const, b const already handled in algebraic; here
   * reassoc (a + k1) + (b + k2) is out of scope. Reassoc a + (b + k) →
   * (a + b) + k when k const for better fold.
   */
  if (!f || f->next_value <= 0)
    return true;
  bool *known = calloc((size_t)f->next_value, sizeof(bool));
  int64_t *value = calloc((size_t)f->next_value, sizeof(int64_t));
  int *def_idx = malloc((size_t)f->next_value * sizeof(*def_idx));
  if (!known || !value || !def_idx) {
    free(known);
    free(value);
    free(def_idx);
    return false;
  }
  memset(def_idx, -1, (size_t)f->next_value * sizeof(*def_idx));

  for (size_t i = 0; i < f->len; ++i) {
    nyir_inst_t *in = &f->data[i];
    if (in->dst >= 0 && (size_t)in->dst < (size_t)f->next_value)
      def_idx[in->dst] = (int)i;
    if (in->op == NYIR_CONST_I64 && in->dst >= 0 &&
        (size_t)in->dst < (size_t)f->next_value) {
      known[in->dst] = true;
      value[in->dst] = in->imm;
    }
    if (in->op != NYIR_ADD_I64 || in->a < 0 || in->b < 0)
      continue;
    /*
     * a + (b + k) where right is add with const
     */
    int j = (in->b < f->next_value) ? def_idx[in->b] : -1;
    if (j >= 0 && (size_t)j < i) {
      nyir_inst_t *r = &f->data[j];
      if (r->op == NYIR_ADD_I64 && r->dst == in->b && r->a >= 0 && r->b >= 0 &&
          known[r->b]) {
        int tdst = f->next_value++;
        if (f->len + 1 > f->cap) {
          size_t nc = f->cap ? f->cap * 2 : 64;
          nyir_inst_t *nd = ny_realloc_array(f->data, nc, sizeof(*nd));
          if (!nd) {
            free(known);
            free(value);
            free(def_idx);
            return false;
          }
          f->data = nd;
          f->cap = nc;
          in = &f->data[i];
          r = &f->data[j];
        }
        memmove(&f->data[i + 1], &f->data[i],
                (f->len - i) * sizeof(nyir_inst_t));
        f->len++;
        int adst = in->dst;
        int aa = in->a;
        f->data[i] =
            (nyir_inst_t){.op = NYIR_ADD_I64, .dst = tdst, .a = aa, .b = r->a};
        f->data[i + 1] = (nyir_inst_t){
            .op = NYIR_ADD_I64, .dst = adst, .a = tdst, .b = r->b};
        i++;
      }
    }
  }
  free(known);
  free(value);
  free(def_idx);
  return true;
}

bool nyir_rewrite_fuel(nyir_func_t *f) {
  /*
   * Run the local rewrite group to a structural fixed point.  Instruction
   * count is not a sufficient progress test: reassociation and algebraic
   * canonicalization can change operands/opcodes without changing f->len,
   * while two individually-correct rules can also oscillate between equal-
   * sized forms.  Fingerprints let us stop on both no-progress and cycles.
   *
   * The bounded history remains a hard safety/resource limit for malformed
   * or unexpectedly adversarial IR, but normal termination is driven by
   * useful structural progress rather than a fixed number of blind repeats.
   */
  if (!f)
    return true;
  enum { NYIR_REWRITE_HISTORY = 16 };
  uint64_t seen[NYIR_REWRITE_HISTORY] = {0};
  size_t seen_len = 0;
  seen[seen_len++] = nyir_debug_fingerprint(f);

  for (;;) {
    if (!nyir_apply_rules(f) || !nyir_double_neg(f) ||
        !nyir_algebraic_combine(f) || !nyir_reassoc_add(f))
      return false;

    uint64_t after = nyir_debug_fingerprint(f);
    if (after == seen[seen_len - 1])
      break;

    bool cycle = false;
    for (size_t i = 0; i < seen_len; ++i) {
      if (seen[i] == after) {
        cycle = true;
        break;
      }
    }
    if (cycle)
      break;
    if (seen_len == NYIR_REWRITE_HISTORY)
      break;
    seen[seen_len++] = after;
  }
  return true;
}

static bool nyir_tbuf_const_before(const nyir_func_t *f, int value,
                                    size_t before, int64_t *out) {
  if (!f || value < 0 || !out)
    return false;
  for (size_t i = 0; i < before && i < f->len; ++i) {
    const nyir_inst_t *in = &f->data[i];
    if (in->dst == value) {
      if (in->op == NYIR_CONST_I64) {
        *out = in->imm;
        return true;
      }
      if (in->op == NYIR_COPY)
        return nyir_tbuf_const_before(f, in->a, i, out);
      if (in->op == NYIR_MUL_I64) {
        int64_t ca = 0, cb = 0;
        if (nyir_tbuf_const_before(f, in->a, i, &ca) &&
            nyir_tbuf_const_before(f, in->b, i, &cb)) {
          *out = ca * cb;
          return true;
        }
      }
      if (in->op == NYIR_ADD_I64) {
        int64_t ca = 0, cb = 0;
        if (nyir_tbuf_const_before(f, in->a, i, &ca) &&
            nyir_tbuf_const_before(f, in->b, i, &cb)) {
          *out = ca + cb;
          return true;
        }
      }
    }
  }
  return false;
}

static bool nyir_tbuf_uses_exact(const nyir_use_def_t *uses, int value,
                                 size_t first, size_t second,
                                 bool two) {
  if (!uses || value < 0 || (size_t)value >= uses->value_count)
    return false;
  size_t begin = uses->offsets[value];
  size_t end = uses->offsets[value + 1];
  if (end - begin != (two ? 2u : 1u))
    return false;
  bool have_first = false, have_second = !two;
  for (size_t i = begin; i < end; ++i) {
    if (uses->users[i] == first)
      have_first = true;
    else if (two && uses->users[i] == second)
      have_second = true;
    else
      return false;
  }
  return have_first && have_second;
}

static bool nyir_tbuf_linear_between(const nyir_func_t *f, size_t begin,
                                     size_t end) {
  if (!f || begin > end || end > f->len)
    return false;
  for (size_t i = begin; i < end; ++i) {
    nyir_op_t op = f->data[i].op;
    if (op == NYIR_CALL || op == NYIR_LABEL || op == NYIR_BR ||
        op == NYIR_BR_IF || op == NYIR_PHI || op == NYIR_RET)
      return false;
  }
  return true;
}
/*
 * Scalarize a non-escaping zero-length tbuf append chain followed only by a
 * length query.  Every intermediate pointer must be consumed exactly once by
 * the next append (and the final pointer exactly once by len), so removing the
 * private allocation/mutations cannot change observable state.  Values passed
 * to append remain evaluated normally; only the dead managed-buffer traffic is
 * removed.
 */
bool nyir_tbuf_scalar_len(nyir_func_t *f) {
  if (!f || f->len == 0)
    return true;
  nyir_use_def_t uses = {0};
  if (!nyir_build_use_def(f, &uses))
    return false;

  for (size_t ni = 0; ni < f->len; ++ni) {
    nyir_inst_t *alloc = &f->data[ni];
    if (alloc->op != NYIR_CALL || !alloc->symbol ||
        strcmp(alloc->symbol, "rt_native_tbuf_new") != 0 || alloc->dst < 0)
      continue;
    int64_t count = 0, elem_size = 0;
    if (alloc->a < 0 || alloc->b < 0 ||
        !nyir_tbuf_const_before(f, alloc->a, ni, &count) ||
        !nyir_tbuf_const_before(f, alloc->b, ni, &elem_size) ||
        count != 0 || elem_size <= 0)
      continue;

    /*
     * new(0, width) followed only by len is exactly zero even when the
     * allocation itself fails (rt_native_tbuf_len(0) == 0).  Unlike an
     * append-chain fold, this therefore preserves the runtime's null/OOM
     * behavior while eliminating both the dead allocation and helper call.
     */
    if (count == 0 && (size_t)alloc->dst < uses.value_count) {
      size_t begin = uses.offsets[alloc->dst];
      size_t end = uses.offsets[alloc->dst + 1];
      if (end - begin == 1u) {
        size_t user_idx = uses.users[begin];
        if (user_idx < f->len) {
          nyir_inst_t *user = &f->data[user_idx];
          if (user->op == NYIR_CALL && user->symbol &&
              strcmp(user->symbol, "rt_native_tbuf_len") == 0 &&
              user->a == alloc->dst && user->dst >= 0) {
            int dst = user->dst;
            nyir_inst_discard(user);
            f->data[user_idx] = (nyir_inst_t){.op = NYIR_CONST_I64,
                                              .dst = dst,
                                              .a = -1, .b = -1, .c = -1,
                                              .d = -1, .e = -1, .f = -1,
                                              .imm = 0};
            nyir_inst_discard(alloc);
            nyir_use_def_free(&uses);
            return true;
          }
        }
      }
    }

    size_t cap = 8, append_len = 0;
    size_t *append_idx = malloc(cap * sizeof(*append_idx));
    int *chain_values = malloc((cap + 1u) * sizeof(*chain_values));
    if (!append_idx || !chain_values) {
      free(append_idx);
      free(chain_values);
      nyir_use_def_free(&uses);
      return false;
    }
    chain_values[0] = alloc->dst;
    int current = alloc->dst;
    size_t last = ni;
    bool committed = false;

    for (size_t i = ni + 1; i < f->len; ++i) {
      nyir_inst_t *in = &f->data[i];
      if (in->op == NYIR_LABEL || in->op == NYIR_BR || in->op == NYIR_BR_IF ||
          in->op == NYIR_PHI || in->op == NYIR_RET)
        break;
      if (in->op != NYIR_CALL)
        continue;
      if (!in->symbol)
        break;

      bool is_append = strcmp(in->symbol, "rt_native_tbuf_append") == 0 ||
                       strcmp(in->symbol, "rt_native_tbuf_append_i64") == 0;
      if (is_append && in->a == current && in->dst >= 0 && in->dst != current &&
          nyir_tbuf_linear_between(f, last + 1u, i)) {
        if (append_len == cap) {
          size_t new_cap = cap * 2u;
          size_t *new_idx = realloc(append_idx, new_cap * sizeof(*new_idx));
          if (!new_idx) {
            free(append_idx);
            free(chain_values);
            nyir_use_def_free(&uses);
            return false;
          }
          append_idx = new_idx;
          int *new_values = realloc(chain_values, (new_cap + 1u) * sizeof(*new_values));
          if (!new_values) {
            free(append_idx);
            free(chain_values);
            nyir_use_def_free(&uses);
            return false;
          }
          chain_values = new_values;
          cap = new_cap;
        }
        append_idx[append_len] = i;
        chain_values[append_len + 1u] = in->dst;
        ++append_len;
        current = in->dst;
        last = i;
        continue;
      }

      if (append_len > 0 && strcmp(in->symbol, "rt_native_tbuf_len") == 0 &&
          in->a == current && in->dst >= 0 &&
          nyir_tbuf_linear_between(f, last + 1u, i)) {
        bool safe = true;
        for (size_t a = 0; a < append_len; ++a) {
          size_t user = append_idx[a];
          if (!nyir_tbuf_uses_exact(&uses, chain_values[a], user, user, false)) {
            safe = false;
            break;
          }
        }
        if (safe && !nyir_tbuf_uses_exact(&uses, chain_values[append_len], i, i, false))
          safe = false;
        if (!safe)
          break;

        int dst = in->dst;
        nyir_inst_discard(in);
        f->data[i] = (nyir_inst_t){.op = NYIR_CONST_I64, .dst = dst,
                                   .a = -1, .b = -1, .c = -1, .d = -1,
                                   .e = -1, .f = -1,
                                   .imm = (int64_t)append_len};
        for (size_t a = 0; a < append_len; ++a)
          nyir_inst_discard(&f->data[append_idx[a]]);
        nyir_inst_discard(alloc);
        committed = true;
        break;
      }

      /*
       * Any other call can observe/perturb state, so this chain is not local.
       */
      break;
    }
    free(append_idx);
    free(chain_values);
    if (committed) {
      nyir_use_def_free(&uses);
      return true;
    }
  }
  nyir_use_def_free(&uses);
  return true;
}

static bool nyir_tbuf_eval_const_offset(const nyir_func_t *f, int v, int base, int64_t *out_off, int depth) {
  if (depth > 16) return false;
  if (base >= 0 && v == base) { *out_off = 0; return true; }
  if (v < 0 || v >= f->next_value) return false;
  for (size_t i = 0; i < f->len; ++i) {
    if (f->data[i].dst == v) {
      const nyir_inst_t *in = &f->data[i];
      if (in->op == NYIR_COPY && in->a >= 0)
        return nyir_tbuf_eval_const_offset(f, in->a, base, out_off, depth + 1);
      if (in->op == NYIR_ADD_I64) {
        int64_t off1 = 0, off2 = 0;
        if (nyir_tbuf_eval_const_offset(f, in->a, base, &off1, depth + 1) &&
            nyir_tbuf_eval_const_offset(f, in->b, -1, &off2, depth + 1)) {
          *out_off = off1 + off2;
          return true;
        }
        if (nyir_tbuf_eval_const_offset(f, in->b, base, &off1, depth + 1) &&
            nyir_tbuf_eval_const_offset(f, in->a, -1, &off2, depth + 1)) {
          *out_off = off1 + off2;
          return true;
        }
      }
      if (in->op == NYIR_CONST_I64 && base == -1) {
        *out_off = in->imm;
        return true;
      }
      if (in->op == NYIR_MUL_I64 && base == -1) {
        int64_t c1 = 0, c2 = 0;
        if (nyir_tbuf_eval_const_offset(f, in->a, -1, &c1, depth + 1) &&
            nyir_tbuf_eval_const_offset(f, in->b, -1, &c2, depth + 1)) {
          *out_off = c1 * c2;
          return true;
        }
      }
      if (in->op == NYIR_SHL_I64 && base == -1) {
        int64_t c1 = 0, c2 = 0;
        if (nyir_tbuf_eval_const_offset(f, in->a, -1, &c1, depth + 1) &&
            nyir_tbuf_eval_const_offset(f, in->b, -1, &c2, depth + 1) && c2 >= 0 && c2 < 64) {
          *out_off = c1 << c2;
          return true;
        }
      }
      break;
    }
  }
  return false;
}

static bool nyir_tbuf_same_addr(const nyir_func_t *f, int a, int b) {
  if (a == b) return true;
  if (a < 0 || b < 0 || a >= f->next_value || b >= f->next_value) return false;
  for (int depth = 0; depth < 16; ++depth) {
    int def_a = -1;
    for (size_t i = 0; i < f->len; ++i) {
      if (f->data[i].dst == a) { def_a = (int)i; break; }
    }
    if (def_a >= 0 && f->data[def_a].op == NYIR_COPY && f->data[def_a].a >= 0) {
      a = f->data[def_a].a;
      if (a == b) return true;
    } else if (def_a >= 0 && f->data[def_a].op == NYIR_LOAD_LOCAL && f->data[def_a].imm >= 0) {
      int64_t slot = f->data[def_a].imm;
      int found_val = -1;
      for (size_t k = (size_t)def_a; k > 0; --k) {
        if (f->data[k - 1].op == NYIR_STORE_LOCAL && f->data[k - 1].imm == slot && f->data[k - 1].a >= 0) {
          found_val = f->data[k - 1].a;
          break;
        }
      }
      if (found_val >= 0) {
        a = found_val;
        if (a == b) return true;
      } else break;
    } else break;
  }
  for (int depth = 0; depth < 16; ++depth) {
    int def_b = -1;
    for (size_t i = 0; i < f->len; ++i) {
      if (f->data[i].dst == b) { def_b = (int)i; break; }
    }
    if (def_b >= 0 && f->data[def_b].op == NYIR_COPY && f->data[def_b].a >= 0) {
      b = f->data[def_b].a;
      if (a == b) return true;
    } else if (def_b >= 0 && f->data[def_b].op == NYIR_LOAD_LOCAL && f->data[def_b].imm >= 0) {
      int64_t slot = f->data[def_b].imm;
      int found_val = -1;
      for (size_t k = (size_t)def_b; k > 0; --k) {
        if (f->data[k - 1].op == NYIR_STORE_LOCAL && f->data[k - 1].imm == slot && f->data[k - 1].a >= 0) {
          found_val = f->data[k - 1].a;
          break;
        }
      }
      if (found_val >= 0) {
        b = found_val;
        if (a == b) return true;
      } else break;
    } else break;
  }
  if (a == b) return true;

  /*
   * Check offset evaluation against shared base allocation
   */
  for (size_t i = 0; i < f->len; ++i) {
    if (f->data[i].op == NYIR_CALL && f->data[i].symbol &&
        strcmp(f->data[i].symbol, "rt_native_tbuf_new") == 0 &&
        f->data[i].dst >= 0) {
      int base = f->data[i].dst;
      int64_t off_a = 0, off_b = 0;
      if (nyir_tbuf_eval_const_offset(f, a, base, &off_a, 0) &&
          nyir_tbuf_eval_const_offset(f, b, base, &off_b, 0) &&
          off_a == off_b)
        return true;
    }
  }
  return false;
}

/*
 * Remove a private dynamic list object when optimization has already proven
 * that every use is only address formation followed by stores.  This is the
 * named-temporary form of escape scalar replacement: the value loaded from
 * `p[0]` may already have been forwarded, leaving only the allocation and its
 * dead initialization stores behind.
 */
bool nyir_tbuf_private_object(nyir_func_t *f) {
  if (!f || f->len == 0 || f->next_value <= 0)
    return true;
  nyir_use_def_t uses = {0};
  if (!nyir_build_use_def(f, &uses))
    return false;
  size_t value_count = (size_t)f->next_value;
  for (size_t alloc_idx = 0; alloc_idx < f->len; ++alloc_idx) {
    nyir_inst_t *alloc = &f->data[alloc_idx];
    if (alloc->op != NYIR_CALL || !alloc->symbol ||
        strcmp(alloc->symbol, "rt_native_tbuf_new") != 0 ||
        alloc->dst < 0 || (size_t)alloc->dst >= value_count)
      continue;
    bool *derived = calloc(value_count, sizeof(*derived));
    bool *seen = calloc(f->len, sizeof(*seen));
    int *queue = malloc(value_count * sizeof(*queue));
    if (!derived || !seen || !queue) {
      free(derived);
      free(seen);
      free(queue);
      nyir_use_def_free(&uses);
      return false;
    }
    size_t queue_len = 0, queue_pos = 0;
    bool has_store = false;
    derived[alloc->dst] = true;
    queue[queue_len++] = alloc->dst;
    bool safe = true;
    while (safe && queue_pos < queue_len) {
      int value = queue[queue_pos++];
      size_t begin = uses.offsets[value];
      size_t end = uses.offsets[value + 1];
      for (size_t ui = begin; ui < end && safe; ++ui) {
        size_t user_idx = uses.users[ui];
        if (user_idx == alloc_idx || user_idx >= f->len)
          continue;
        nyir_inst_t *in = &f->data[user_idx];
        bool a = in->a == value;
        bool b = in->b == value;
        bool c = in->c == value;
        if (in->op == NYIR_COPY && a && !b && !c && in->dst >= 0 &&
            (size_t)in->dst < value_count) {
          if (!derived[in->dst]) {
            derived[in->dst] = true;
            queue[queue_len++] = in->dst;
          }
        } else if ((in->op == NYIR_ADD_I64 || in->op == NYIR_SUB_I64) &&
                   (a != b) && (a || b) && in->dst >= 0 &&
                   (size_t)in->dst < value_count) {
          int other = a ? in->b : in->a;
          int64_t constant = 0;
          if (other < 0 ||
              !nyir_tbuf_const_before(f, other, user_idx, &constant)) {
            safe = false;
            break;
          }
          if (!derived[in->dst]) {
            derived[in->dst] = true;
            queue[queue_len++] = in->dst;
          }
        } else if (in->op == NYIR_STORE_I64 && a && in->c >= 0 &&
                   !derived[in->c]) {
          seen[user_idx] = true;
          has_store = true;
        } else if (in->op == NYIR_CALL && a && !b && !c && in->symbol &&
                   strcmp(in->symbol, "rt_native_tbuf_len") == 0) {
          int64_t off = 0;
          /*
           * A private tbuf whose only operations are address formation,
           * payload stores/loads and length reads cannot have its count
           * changed.  Replace an exact-base length query with the allocator's
           * count SSA value.  Do not mark the rewritten COPY as dead: it
           * defines the original call result just like forwarded loads below.
           */
          if (nyir_tbuf_eval_const_offset(f, in->a, alloc->dst, &off, 0) &&
              off == 0 && alloc->a >= 0) {
            nir_make_copy(in, alloc->a);
          } else {
            safe = false;
          }
        } else if (in->op == NYIR_LOAD_I64 && a) {
          int stored_val = -1;
          for (size_t k = user_idx; k > alloc_idx; --k) {
            const nyir_inst_t *prev = &f->data[k - 1];
            if (prev->op == NYIR_LABEL || prev->op == NYIR_BR ||
                prev->op == NYIR_BR_IF)
              break;
            if (prev->op == NYIR_STORE_I64 &&
                (prev->a == in->a || nyir_tbuf_same_addr(f, prev->a, in->a))) {
              stored_val = prev->c;
              break;
            }
          }
          if (stored_val >= 0) {
            /*
             * Keep the forwarded COPY: it defines the original load result.
             * Only the dead object/address/store scaffolding is discarded.
             */
nir_make_copy(in, stored_val);
          } else {
            safe = false;
          }
        } else if (in->op == NYIR_STORE_LOCAL && a && in->imm >= 0) {
          seen[user_idx] = true;
          int64_t slot = in->imm;
          for (size_t k = 0; k < f->len; ++k) {
            if (f->data[k].op == NYIR_LOAD_LOCAL && f->data[k].imm == slot &&
                f->data[k].dst >= 0 && (size_t)f->data[k].dst < value_count) {
              seen[k] = true;
              if (!derived[f->data[k].dst]) {
                derived[f->data[k].dst] = true;
                queue[queue_len++] = f->data[k].dst;
              }
            }
          }
        } else if (in->op == NYIR_BOUNDS_CHECK && a) {
          seen[user_idx] = true;
        } else {
          safe = false;
        }
        if (safe && in->dst >= 0 && (size_t)in->dst < value_count &&
            derived[in->dst] &&
            (in->op == NYIR_COPY || in->op == NYIR_ADD_I64 ||
             in->op == NYIR_SUB_I64))
          seen[user_idx] = true;
      }
    }
    if (safe && has_store) {
      for (size_t i = 0; i < f->len; ++i)
        if (i == alloc_idx || seen[i] ||
            (f->data[i].dst >= 0 &&
             (size_t)f->data[i].dst < value_count &&
             derived[f->data[i].dst]))
          nyir_inst_discard(&f->data[i]);
    }
    free(derived);
    free(seen);
    free(queue);
    if (!safe || !has_store)
      continue;
  }
  nyir_use_def_free(&uses);
  return true;
}

/*
 * Apply NyP1 profile: boost inline candidate size when edge density is high.
 */
static uint64_t ny_profile_edges;
static uint64_t ny_profile_steps;

void ny_native_profile_set_runtime(uint64_t edges, uint64_t steps) {
  ny_profile_edges = edges;
  ny_profile_steps = steps;
}

bool ny_native_profile_should_inline(size_t callee_insts) {
  /*
   * Hot profiles allow larger callees (up to 32 insts). NyP-style density.
   */
  size_t lim = 12;
  if (ny_profile_steps > 10000 && ny_profile_edges > 100)
    lim = 32;
  else if (ny_profile_steps > 1000)
    lim = 20;
  return callee_insts > 0 && callee_insts <= lim;
}

/*
 * Dense NyP-style ICP: count monomorphic direct CALL targets in the function.
 * When a single symbol dominates (>= 2 sites, or unique among multi-call),
 * boost profile so inline_small admits that callee's body.
 */
bool nyir_icp_profile(nyir_func_t *f) {
  if (!f || f->len == 0)
    return true;
  /*
   * Collect up to 16 distinct direct call symbols and counts.
   */
  const char *syms[16];
  unsigned counts[16];
  size_t nsym = 0;
  size_t total_calls = 0;
  for (size_t i = 0; i < f->len; ++i) {
    if (f->data[i].op != NYIR_CALL || !f->data[i].symbol)
      continue;
    total_calls++;
    const char *s = f->data[i].symbol;
    size_t j;
    for (j = 0; j < nsym; ++j)
      if (strcmp(syms[j], s) == 0) {
        counts[j]++;
        break;
      }
    if (j == nsym && nsym < 16) {
      syms[nsym] = s;
      counts[nsym] = 1;
      nsym++;
    }
  }
  if (total_calls == 0)
    return true;
  /*
   * Monomorphic: one target, or one target has >= 2/3 of direct calls.
   */
  unsigned best = 0;
  for (size_t j = 0; j < nsym; ++j)
    if (counts[j] > best)
      best = counts[j];
  bool mono = (nsym == 1 && total_calls >= 1) ||
              (best * 3 >= (unsigned)total_calls * 2 && best >= 2);
  if (mono) {
    uint64_t steps = ny_profile_steps < 2000 ? 2000 : ny_profile_steps;
    uint64_t edges = ny_profile_edges < 200 ? 200 : ny_profile_edges + best * 10;
    ny_native_profile_set_runtime(edges, steps);
  } else if (ny_profile_steps >= 100 && ny_profile_edges < 200) {
    ny_native_profile_set_runtime(ny_profile_edges + 100, ny_profile_steps);
  }
  return true;
}

uint64_t ny_native_profile_edges(void) { return ny_profile_edges; }
uint64_t ny_native_profile_steps(void) { return ny_profile_steps; }

/*
 * Hot/cold layout: invert br_if when the cold arm is a tiny ret/exit and the
 * true arm is larger so fallthrough hits the hot body. Also prefer the larger
 * arm as fallthrough when profile heat suggests inlining-scale bodies
 * (NyP-style edge preference without full ICP).
 */
static bool nyir_invert_br_if_at(nyir_func_t *f, size_t i) {
  if (!f || i + 1 >= f->len || f->data[i].op != NYIR_BR_IF ||
      f->data[i + 1].op != NYIR_BR)
    return false;
  int cond = f->data[i].a;
  int64_t lt = f->data[i].imm, lf = f->data[i + 1].imm;
  for (size_t j = 0; j < i; ++j) {
    if (f->data[j].op == NYIR_CMP_I64 && f->data[j].dst == cond) {
      nyir_cmp_t c = f->data[j].cmp;
      nyir_cmp_t inv = c;
      switch (c) {
      case NYIR_CMP_EQ:
        inv = NYIR_CMP_NE;
        break;
      case NYIR_CMP_NE:
        inv = NYIR_CMP_EQ;
        break;
      case NYIR_CMP_LT:
        inv = NYIR_CMP_GE;
        break;
      case NYIR_CMP_LE:
        inv = NYIR_CMP_GT;
        break;
      case NYIR_CMP_GT:
        inv = NYIR_CMP_LE;
        break;
      case NYIR_CMP_GE:
        inv = NYIR_CMP_LT;
        break;
      default:
        inv = c;
        break;
      }
      if (inv == c)
        return false;
      f->data[j].cmp = inv;
      f->data[i].imm = lf;
      f->data[i + 1].imm = lt;
      return true;
    }
  }
  return false;
}

/*
 * Aggregate/field SROA: (1) dead-member store kill; (2) within straight-line
 * regions promote non-escaped store→load to COPY (field-scalar); (3) kill
 * stores that are overwritten before any load of that member.
 */
bool nyir_aggregate_sroa(nyir_func_t *f) {
  if (!f || f->len == 0)
    return true;
  size_t n = nyir_max_local(f);
  if (!n)
    return true;
  bool *loaded = calloc(n, 1);
  bool *escaped = calloc(n, 1);
  int *last_store = ny_malloc_array(n, sizeof(int));
  int *field_def = ny_malloc_array(n, sizeof(int));
  if (!loaded || !escaped || !last_store || !field_def) {
    free(loaded);
    free(escaped);
    free(last_store);
    free(field_def);
    return false;
  }
  for (size_t i = 0; i < n; ++i) {
    last_store[i] = -1;
    field_def[i] = -1;
  }
  for (size_t i = 0; i < f->len; ++i) {
    if (f->data[i].op == NYIR_LOAD_LOCAL && f->data[i].imm >= 0 &&
        (size_t)f->data[i].imm < n)
      loaded[f->data[i].imm] = true;
    if (f->data[i].op == NYIR_ADDR_LOCAL && f->data[i].imm >= 0 &&
        (size_t)f->data[i].imm < n)
      escaped[f->data[i].imm] = true;
  }
  /*
   * Global dead-member stores.
   */
  for (size_t i = 0; i < f->len; ++i) {
    nyir_inst_t *in = &f->data[i];
    if (in->op == NYIR_STORE_LOCAL && in->imm >= 0 && (size_t)in->imm < n &&
        !escaped[in->imm] && !loaded[in->imm])
      *in = (nyir_inst_t){.op = NYIR_NOP, .dst = -1, .a = -1, .b = -1};
  }
  /*
   * Straight-line field promote + kill overwritten stores.
   */
  for (size_t i = 0; i < f->len; ++i) {
    nyir_inst_t *in = &f->data[i];
    if (in->op == NYIR_LABEL || in->op == NYIR_BR || in->op == NYIR_BR_IF ||
        in->op == NYIR_RET) {
      for (size_t l = 0; l < n; ++l) {
        last_store[l] = -1;
        field_def[l] = -1;
      }
      continue;
    }
    if (in->op == NYIR_CALL) {
      for (size_t l = 0; l < n; ++l) {
        if (escaped[l]) {
          last_store[l] = -1;
          field_def[l] = -1;
        }
      }
      continue;
    }
    if (in->op == NYIR_STORE_LOCAL && in->imm >= 0 && (size_t)in->imm < n &&
        !escaped[in->imm]) {
      if (last_store[in->imm] >= 0) {
        size_t prev = (size_t)last_store[in->imm];
        f->data[prev] =
            (nyir_inst_t){.op = NYIR_NOP, .dst = -1, .a = -1, .b = -1};
      }
      last_store[in->imm] = (int)i;
      field_def[in->imm] = in->a;
    } else if (in->op == NYIR_LOAD_LOCAL && in->imm >= 0 &&
               (size_t)in->imm < n && !escaped[in->imm] &&
               field_def[in->imm] >= 0 && in->dst >= 0) {
      *in = (nyir_inst_t){
          .op = NYIR_COPY, .dst = in->dst, .a = field_def[in->imm], .b = -1};
      last_store[in->imm] = -1;
    }
  }
  free(loaded);
  free(escaped);
  free(last_store);
  free(field_def);
  return true;
}

/*
 * Dead store after store to same non-escaped local with no intervening load.
 */
bool nyir_store_sink(nyir_func_t *f) {
  if (!f || f->len == 0)
    return true;
  size_t n = nyir_max_local(f);
  if (!n)
    return true;
  int *last_store = ny_malloc_array(n, sizeof(int));
  bool *escaped = calloc(n, 1);
  if (!last_store || !escaped) {
    free(last_store);
    free(escaped);
    return false;
  }
  for (size_t i = 0; i < n; ++i)
    last_store[i] = -1;
  for (size_t i = 0; i < f->len; ++i)
    if (f->data[i].op == NYIR_ADDR_LOCAL && f->data[i].imm >= 0 &&
        (size_t)f->data[i].imm < n)
      escaped[f->data[i].imm] = true;
  for (size_t i = 0; i < f->len; ++i) {
    nyir_inst_t *in = &f->data[i];
    if (in->op == NYIR_LABEL || in->op == NYIR_BR || in->op == NYIR_BR_IF ||
        in->op == NYIR_CALL || in->op == NYIR_RET) {
      for (size_t l = 0; l < n; ++l)
        last_store[l] = -1;
      continue;
    }
    if (in->op == NYIR_LOAD_LOCAL && in->imm >= 0 && (size_t)in->imm < n)
      last_store[in->imm] = -1;
    else if (in->op == NYIR_STORE_LOCAL && in->imm >= 0 &&
             (size_t)in->imm < n && !escaped[in->imm]) {
      if (last_store[in->imm] >= 0) {
        size_t prev = (size_t)last_store[in->imm];
        f->data[prev] =
            (nyir_inst_t){.op = NYIR_NOP, .dst = -1, .a = -1, .b = -1};
      }
      last_store[in->imm] = (int)i;
    }
  }
  free(last_store);
  free(escaped);
  return true;
}

/*
 * Local points-to seed: ADDR_LOCAL v → local L. When LOAD_I64 of that pointer
 * and L is never passed to CALL, rewrite to LOAD_LOCAL. Complements SROA.
 */
bool nyir_points_to_sroa(nyir_func_t *f) {
  if (!f || f->len == 0 || f->next_value <= 0)
    return true;
  size_t nv = (size_t)f->next_value;
  int *pt = ny_malloc_array(nv, sizeof(int));
  bool *escaped = NULL;
  if (!pt)
    return false;
  for (size_t i = 0; i < nv; ++i)
    pt[i] = -1;
  for (size_t i = 0; i < f->len; ++i) {
    const nyir_inst_t *in = &f->data[i];
    if (in->op == NYIR_ADDR_LOCAL && in->dst >= 0 && in->imm >= 0) {
      pt[in->dst] = (int)in->imm;
    } else if (in->op == NYIR_COPY && in->dst >= 0 && in->a >= 0 &&
               (size_t)in->a < nv && pt[in->a] >= 0) {
      pt[in->dst] = pt[in->a];
    }
  }
  size_t max_local = nyir_max_local(f);
  if (max_local == 0) {
    free(pt);
    return true;
  }
  escaped = calloc(max_local, 1);
  if (!escaped) {
    free(pt);
    return false;
  }
  for (size_t i = 0; i < f->len; ++i) {
    const nyir_inst_t *in = &f->data[i];
    if (in->op != NYIR_CALL)
      continue;
    int args[16];
    int argc = 0;
    if (!nyir_call_args(in, f->next_value, args, 16, &argc, NULL, 0))
      continue;
    for (int a = 0; a < argc; ++a) {
      if (args[a] >= 0 && (size_t)args[a] < nv && pt[args[a]] >= 0)
        escaped[pt[args[a]]] = true;
    }
  }
  for (size_t i = 0; i < f->len; ++i) {
    nyir_inst_t *in = &f->data[i];
    if (in->op == NYIR_LOAD_I64 && in->a >= 0 && (size_t)in->a < nv &&
        pt[in->a] >= 0 && !escaped[pt[in->a]] && in->dst >= 0) {
      *in = (nyir_inst_t){.op = NYIR_LOAD_LOCAL,
                            .dst = in->dst,
                            .a = -1,
                            .b = -1,
                            .imm = pt[in->a]};
    } else if (in->op == NYIR_STORE_I64 && in->a >= 0 && (size_t)in->a < nv &&
               pt[in->a] >= 0 && !escaped[pt[in->a]] && in->c >= 0) {
      /*
       * store *addr, val (a=addr, c=val) → store.local
       */
      *in = (nyir_inst_t){.op = NYIR_STORE_LOCAL,
                            .dst = -1,
                            .a = in->c,
                            .b = -1,
                            .c = -1,
                            .imm = pt[in->a]};
    }
  }
  free(pt);
  free(escaped);
  return true;
}

static void nyir_polyhedral_descending_ranges(nyir_func_t *f) {
  if (!f || f->len < 8)
    return;
  for (size_t cmp_i = 0; cmp_i < f->len; ++cmp_i) {
    nyir_inst_t *cmp = &f->data[cmp_i];
    if (cmp->op != NYIR_CMP_I64 || cmp->cmp != NYIR_CMP_GT ||
        cmp->a < 0 || cmp->b < 0)
      continue;
    int iv = cmp->a;
    int64_t limit = 0;
    bool limit_known = false;
    for (size_t j = 0; j < f->len; ++j) {
      if (f->data[j].dst == cmp->b && f->data[j].op == NYIR_CONST_I64) {
        limit = f->data[j].imm;
        limit_known = true;
        break;
      }
    }
    if (!limit_known)
      continue;

    int back_value = -1;
    int64_t entry = 0;
    bool entry_known = false;
    for (size_t j = 0; j < cmp_i; ++j) {
      nyir_inst_t *phi = &f->data[j];
      if (phi->op != NYIR_PHI || phi->dst != iv ||
          phi->phi_incoming_len != 2 || !phi->phi_incoming)
        continue;
      for (size_t k = 0; k < phi->phi_incoming_len; ++k) {
        int v = phi->phi_incoming[k].value;
        bool is_const = false;
        int64_t c = 0;
        for (size_t m = 0; m < f->len; ++m) {
          if (f->data[m].dst != v)
            continue;
          if (f->data[m].op == NYIR_CONST_I64) {
            is_const = true;
            c = f->data[m].imm;
          }
          break;
        }
        if (is_const) {
          entry = c;
          entry_known = true;
        } else if (back_value < 0) {
          back_value = v;
        }
      }
      break;
    }
    if (!entry_known || back_value < 0 || entry <= limit)
      continue;

    int64_t step = 0;
    bool step_known = false;
    for (size_t j = 0; j < f->len; ++j) {
      const nyir_inst_t *upd = &f->data[j];
      if (upd->dst != back_value)
        continue;
      int step_v = -1;
      if (upd->op == NYIR_SUB_I64 && upd->a == iv)
        step_v = upd->b;
      else if (upd->op == NYIR_ADD_I64) {
        if (upd->a == iv) step_v = upd->b;
        else if (upd->b == iv) step_v = upd->a;
      }
      if (step_v < 0)
        break;
      for (size_t m = 0; m < f->len; ++m) {
        if (f->data[m].dst != step_v || f->data[m].op != NYIR_CONST_I64)
          continue;
        int64_t c = f->data[m].imm;
        if (upd->op == NYIR_SUB_I64 && c > 0) {
          step = c;
          step_known = true;
        } else if (upd->op == NYIR_ADD_I64 && c < 0 && c != INT64_MIN) {
          step = -c;
          step_known = true;
        }
        break;
      }
      break;
    }
    if (!step_known || step <= 0)
      continue;

    __int128 span = (__int128)entry - (__int128)limit;
    __int128 rounds = (span + step - 1) / step;
    __int128 fail = (__int128)entry - rounds * step;
    if (fail < INT64_MIN || fail > INT64_MAX)
      continue;
    int64_t iv_min = (int64_t)fail;
    for (size_t j = 0; j < f->len; ++j) {
      if (f->data[j].dst != iv && f->data[j].dst != back_value)
        continue;
      if (f->data[j].op != NYIR_PHI && f->data[j].op != NYIR_COPY &&
          f->data[j].op != NYIR_SUB_I64 && f->data[j].op != NYIR_ADD_I64)
        continue;
      f->data[j].range.has_min = true;
      f->data[j].range.has_max = true;
      f->data[j].range.min = iv_min;
      f->data[j].range.max = entry;
    }
    cmp->range = (nyir_range_t){.has_min = true, .has_max = true,
                                .min = 0, .max = 1};
  }
}

/*
 * Polyhedral-style nest analysis: counted outer/inner trip, IV ranges, and
 * dependence-free tiling hint (when both trips constant and body is pure
 * affine add/mul on IVs — mark outer IV range so unroll can fire).
 */
bool nyir_polyhedral_nest(nyir_func_t *f) {
  nyir_polyhedral_descending_ranges(f);
  if (!f || f->len < 12)
    return true;
  for (size_t i = 0; i < f->len; ++i) {
    if (f->data[i].op != NYIR_CMP_I64 || f->data[i].cmp != NYIR_CMP_LT)
      continue;
    int iv = f->data[i].a;
    int lim = f->data[i].b;
    if (iv < 0 || lim < 0)
      continue;
    int64_t trip = -1;
    for (size_t j = 0; j < i; ++j)
      if (f->data[j].op == NYIR_CONST_I64 && f->data[j].dst == lim) {
        trip = f->data[j].imm;
        break;
      }
    if (trip < 1 || trip > 1000000)
      continue;
    f->data[i].range.has_min = true;
    f->data[i].range.has_max = true;
    f->data[i].range.min = 0;
    f->data[i].range.max = 1;
    /*
     * Tag the IV defs only for a canonical +1-step induction with a proven
     * non-negative entry value.  The header phi legitimately reaches `trip`
     * on the exit-check iteration (`i == lim` fails `i < lim`), so the sound
     * max is `trip`, never `trip - 1`: a too-low max lets peephole's range
     * fold prove the exit test `i >= lim` always false and delete the loop
     * guard (infinite loop / skipped loop after inlining clones the ranges).
     * Tag the IV defs only for a canonical +1-step induction with a proven
     * non-negative constant start.  The header phi legitimately reaches
     * `trip` on the exit-check iteration (`i == lim` fails `i < lim`), so the
     * sound max is `trip`, never `trip - 1`: a too-low max lets peephole's
     * range fold prove the exit test `i >= lim` always false and delete the
     * loop guard (infinite loop / skipped loop once inlining clones the
     * ranges into the caller).
     *
     * The step add lives in the loop body, AFTER the header's CMP in linear
     * IR order, so the whole function is scanned for the phi's backedge
     * operand instead of only scanning before the CMP.
     */
    int64_t entry_imm = 0;
    bool entry_known = false;
    int64_t step_imm = 0;
    bool positive_step = false;
    int back_op = -1;
    /*
     * Modern NYIR PHIs carry predecessor/value pairs exclusively in
     * phi_incoming; a/b are normalized to -1.  Reading a/b here made IV range
     * discovery silently fail for every SSA-built loop.
     */
    for (size_t j = 0; j < i; ++j) {
      nyir_inst_t *phi = &f->data[j];
      if (phi->dst != iv || phi->op != NYIR_PHI ||
          phi->phi_incoming_len != 2 || !phi->phi_incoming)
        continue;
      for (size_t k = 0; k < phi->phi_incoming_len; ++k) {
        int op = phi->phi_incoming[k].value;
        if (op < 0 || op >= f->next_value)
          continue;
        bool is_const = false;
        int64_t cval = 0;
        for (size_t m = 0; m < f->len; ++m) {
          if (f->data[m].dst != op)
            continue;
          if (f->data[m].op == NYIR_CONST_I64) {
            is_const = true;
            cval = f->data[m].imm;
          }
          break;
        }
        if (is_const && cval >= 0) {
          entry_imm = cval;
          entry_known = true;
        } else if (!is_const && back_op < 0) {
          back_op = op;
        }
      }
      if (entry_known && back_op >= 0) {
        for (size_t m = 0; m < f->len && !positive_step; ++m) {
          const nyir_inst_t *st = &f->data[m];
          if (st->dst != back_op || st->op != NYIR_ADD_I64)
            continue;
          int c_op = st->a == iv ? st->b : (st->b == iv ? st->a : -1);
          if (c_op < 0)
            continue;
          for (size_t n = 0; n < f->len; ++n) {
            if (f->data[n].op != NYIR_CONST_I64 || f->data[n].dst != c_op ||
                f->data[n].imm <= 0)
              continue;
            step_imm = f->data[n].imm;
            positive_step = true;
            break;
          }
        }
      }
      break;
    }
    /*
     * For iv = entry + k*step with iv < limit, the header PHI can reach one
     * failing value at/above the limit.  Compute that exact aligned value
     * rather than assuming unit stride, and attach the same range to the
     * backedge update so derived affine expressions inherit it.
     */
    if (entry_known && positive_step) {
      int64_t iv_max = entry_imm;
      if (entry_imm < trip) {
        uint64_t span = (uint64_t)trip - (uint64_t)entry_imm;
        uint64_t step = (uint64_t)step_imm;
        uint64_t rounds = span / step + (span % step != 0);
        if (rounds <= (uint64_t)INT64_MAX / step) {
          uint64_t advance = rounds * step;
          if (advance <= (uint64_t)INT64_MAX - (uint64_t)entry_imm)
            iv_max = entry_imm + (int64_t)advance;
          else
            positive_step = false;
        } else {
          positive_step = false;
        }
      }
      if (positive_step) {
        for (size_t j = 0; j < f->len; ++j) {
          if (f->data[j].dst != iv && f->data[j].dst != back_op)
            continue;
          if (f->data[j].op == NYIR_COPY || f->data[j].op == NYIR_ADD_I64 ||
              f->data[j].op == NYIR_PHI) {
            f->data[j].range.has_min = true;
            f->data[j].range.has_max = true;
            f->data[j].range.min = entry_imm;
            f->data[j].range.max = iv_max;
          }
        }
      }
    }
    int64_t trip2 = -1;
    for (size_t k = i + 1; k < f->len && k < i + 128; ++k) {
      if (f->data[k].op != NYIR_CMP_I64 || f->data[k].cmp != NYIR_CMP_LT)
        continue;
      int lim2 = f->data[k].b;
      if (lim2 < 0)
        continue;
      for (size_t j = 0; j < k; ++j)
        if (f->data[j].op == NYIR_CONST_I64 && f->data[j].dst == lim2) {
          trip2 = f->data[j].imm;
          break;
        }
      if (trip2 >= 1 && trip2 <= 10000) {
        f->data[k].range.has_min = true;
        f->data[k].range.has_max = true;
        f->data[k].range.min = 0;
        f->data[k].range.max = 1;
        break;
      }
    }
    /*
     * Tiling / unroll hint: small constant trip or nest product ≤ 64 →
     * tag lim const with range so consumers see exact trip; boost profile
     * when product is tile-friendly (power-of-two-ish small).
     */
    if (trip >= 2 && trip <= 8) {
      for (size_t j = 0; j < i; ++j)
        if (f->data[j].op == NYIR_CONST_I64 && f->data[j].dst == lim) {
          f->data[j].range.has_min = true;
          f->data[j].range.has_max = true;
          f->data[j].range.min = trip;
          f->data[j].range.max = trip;
        }
    }
    if (trip2 >= 2 && trip >= 2 && trip * trip2 <= 64) {
      if (ny_profile_steps < 3000)
        ny_native_profile_set_runtime(ny_profile_edges + 20, 3000);
    }
  }
  return true;
}

/*
 * @kernel autotune: CALL targets matching *kernel* → larger inline budget,
 * profile boost proportional to arg-count (proxy for work item), and
 * polyhedral-friendly step count so nest ranges fire before inline.
 */
bool nyir_kernel_hint(nyir_func_t *f) {
  if (!f)
    return true;
  size_t kernel_calls = 0;
  size_t kernel_args = 0;
  for (size_t i = 0; i < f->len; ++i) {
    nyir_inst_t *in = &f->data[i];
    if (in->op != NYIR_CALL || !in->symbol)
      continue;
    if (strstr(in->symbol, "kernel") || strstr(in->symbol, "Kernel") ||
        strstr(in->symbol, "KERNEL")) {
      kernel_calls++;
      int args[16];
      int argc = 0;
      if (nyir_call_args(in, f->next_value, args, 16, &argc, NULL, 0))
        kernel_args += (size_t)(argc > 0 ? argc : 1);
      else
        kernel_args += 1;
    }
  }
  if (kernel_calls == 0)
    return true;
  /*
   * Target pick: more args / more sites → hotter kernel → denser budget.
   */
  uint64_t steps = 5000 + kernel_calls * 500 + kernel_args * 100;
  if (steps > 20000)
    steps = 20000;
  uint64_t edges = ny_profile_edges + 50 + kernel_calls * 25;
  if (ny_profile_steps < steps)
    ny_native_profile_set_runtime(edges, steps);
  return true;
}

bool nyir_block_layout(nyir_func_t *f) {
  if (!f || f->len < 4)
    return true;
  for (size_t i = 0; i + 1 < f->len; ++i) {
    if (f->data[i].op != NYIR_BR_IF || f->data[i + 1].op != NYIR_BR)
      continue;
    int64_t lt = f->data[i].imm, lf = f->data[i + 1].imm;
    size_t t_sz = 0, f_sz = 0;
    bool f_is_ret = false, t_is_ret = false;
    for (size_t j = 0; j < f->len; ++j) {
      if (f->data[j].op == NYIR_LABEL && f->data[j].imm == lt) {
        for (size_t k = j + 1; k < f->len && f->data[k].op != NYIR_LABEL; ++k) {
          t_sz++;
          if (f->data[k].op == NYIR_RET)
            t_is_ret = true;
        }
      }
      if (f->data[j].op == NYIR_LABEL && f->data[j].imm == lf) {
        for (size_t k = j + 1; k < f->len && f->data[k].op != NYIR_LABEL; ++k) {
          f_sz++;
          if (f->data[k].op == NYIR_RET)
            f_is_ret = true;
        }
      }
    }
    size_t t_pc = SIZE_MAX, f_pc = SIZE_MAX;
    for (size_t j = 0; j < f->len; ++j) {
      if (f->data[j].op != NYIR_LABEL)
        continue;
      if (f->data[j].imm == lt)
        t_pc = j;
      if (f->data[j].imm == lf)
        f_pc = j;
    }
    uint64_t t_heat = t_pc != SIZE_MAX
                          ? ny_native_profile_edge_hot(i, t_pc)
                          : 0;
    uint64_t f_heat = f_pc != SIZE_MAX
                          ? ny_native_profile_edge_hot(i + 1, f_pc)
                          : 0;
    if (!f_heat)
      f_heat = ny_native_profile_edge_hot(i, i + 1);
    /*
     * Edge samples are the strongest signal, but older/sparser profiles may
     * contain only per-block or per-loop heat.  Consume those too instead of
     * falling back to block-size heuristics simply because one edge counter is
     * absent.  Saturate additions so corrupted/very hot counters cannot wrap
     * and invert a branch decision.
     */
    if (!t_heat && t_pc != SIZE_MAX) {
      uint64_t block = ny_native_profile_block_hot(t_pc);
      uint64_t loop = ny_native_profile_loop_hot(t_pc);
      t_heat = UINT64_MAX - block < loop ? UINT64_MAX : block + loop;
    }
    if (!f_heat && f_pc != SIZE_MAX) {
      uint64_t block = ny_native_profile_block_hot(f_pc);
      uint64_t loop = ny_native_profile_loop_hot(f_pc);
      f_heat = UINT64_MAX - block < loop ? UINT64_MAX : block + loop;
    }
    if (t_heat || f_heat) {
      if (t_heat > f_heat)
        (void)nyir_invert_br_if_at(f, i);
      continue;
    }

    /*
     * Cold exit on false arm: invert so hot true falls through after invert
     * (true becomes the old false which was cold — wait: we want fallthrough
     * = hot. After invert, fallthrough is the old true. So we invert when
     * fallthrough (false arm) is cold ret and true is hot. That makes old true
     * become fallthrough. Correct.
     */
    if (f_is_ret && f_sz <= 3 && t_sz > f_sz) {
      (void)nyir_invert_br_if_at(f, i);
      continue;
    }
    /*
     * Profile-guided-ish: when true is tiny ret and false is large, leave as-is
     * (fallthrough already hot). When both non-ret and true is much larger
     * than false, invert so large arm falls through after invert... actually
     * fallthrough is false arm; invert swaps so old true becomes fallthrough.
     */
    if (!f_is_ret && !t_is_ret && t_sz >= f_sz * 2 && t_sz >= 4 &&
        ny_native_profile_should_inline(t_sz)) {
      (void)nyir_invert_br_if_at(f, i);
    }
  }
  return true;
}
