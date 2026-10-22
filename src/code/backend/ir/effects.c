#include "code/backend/ir/internal.h"
#include <nyr/ir.h>
#include <string.h>

/*
 * Audited runtime-call effects used by optimization and tier diagnostics.
 *
 * Keep this table conservative: `CALL` is always present for a known helper;
 * `UNKNOWN_SIDE_EFFECT` is reserved for helpers whose externally observable
 * behavior cannot be represented by NYIR's current effect bits.  Read/write
 * helpers retain MAY_TRAP where an invalid raw pointer could fault.  This
 * gives alias/effect passes useful distinctions without pretending that raw
 * pointer accesses have provenance they do not carry. READ_MEMORY and
 * WRITE_MEMORY therefore describe the whole possible memory set, including
 * argument-reachable objects; the summary carries no exact pointer set or
 * write extent. Consumers may only narrow that barrier with a separate proof.
 */
typedef struct {
  const char *name;
  unsigned effects;
} nyr_runtime_effect_entry_t;

static const nyr_runtime_effect_entry_t nyr_runtime_effects[] = {
    /*
     * Deterministic scalar helpers: no memory, allocation, or global state.
     */
    {"rt_tag", NYIR_EFFECT_CALL},
    {"rt_bool_to_cstr", NYIR_EFFECT_CALL},

    /*
     * Stable process state / clock state.
     */
    {"rt_argc", NYIR_EFFECT_CALL | NYIR_EFFECT_READ_MEMORY},
    {"rt_ticks_ns", NYIR_EFFECT_CALL | NYIR_EFFECT_VOLATILE | NYIR_EFFECT_IO},
    {"rt_monotonic_ns_raw",
     NYIR_EFFECT_CALL | NYIR_EFFECT_VOLATILE | NYIR_EFFECT_IO},

    /*
     * Read-only raw/managed storage helpers.
     */
    {"rt_any_to_i64", NYIR_EFFECT_CALL | NYIR_EFFECT_READ_MEMORY},
    {"rt_len",
     NYIR_EFFECT_CALL | NYIR_EFFECT_READ_MEMORY | NYIR_EFFECT_MAY_TRAP},
    {"rt_cstr_len",
     NYIR_EFFECT_CALL | NYIR_EFFECT_READ_MEMORY | NYIR_EFFECT_MAY_TRAP},
    {"rt_cstr_eq",
     NYIR_EFFECT_CALL | NYIR_EFFECT_READ_MEMORY | NYIR_EFFECT_MAY_TRAP},
    /*
     * Reads the dynamic value's object metadata and may allocate only while
     * constructing the type error on its trapping path. Allocation creates
     * fresh objects; it does not mutate an existing symbol cell.
     */
    {"rt_len_strict", NYIR_EFFECT_CALL | NYIR_EFFECT_READ_MEMORY |
                          NYIR_EFFECT_ALLOCATION | NYIR_EFFECT_MAY_TRAP},
    {"rt_tbuf_len_raw",
     NYIR_EFFECT_CALL | NYIR_EFFECT_READ_MEMORY | NYIR_EFFECT_MAY_TRAP},
    {"rt_tbuf_get",
     NYIR_EFFECT_CALL | NYIR_EFFECT_READ_MEMORY | NYIR_EFFECT_MAY_TRAP},
    {"rt_tbuf_tag",
     NYIR_EFFECT_CALL | NYIR_EFFECT_READ_MEMORY | NYIR_EFFECT_MAY_TRAP},
    /*
     * Raw loads observe caller-visible memory and can trap on invalid ranges.
     * Read-only summaries let GVN reuse equal address/index pairs, while
     * stores and unknown calls still invalidate their values.
     */
    {"rt_load8_idx",
     NYIR_EFFECT_CALL | NYIR_EFFECT_READ_MEMORY | NYIR_EFFECT_MAY_TRAP},
    {"rt_load16_idx",
     NYIR_EFFECT_CALL | NYIR_EFFECT_READ_MEMORY | NYIR_EFFECT_MAY_TRAP},
    {"rt_load32_idx",
     NYIR_EFFECT_CALL | NYIR_EFFECT_READ_MEMORY | NYIR_EFFECT_MAY_TRAP},
    {"rt_load32_h",
     NYIR_EFFECT_CALL | NYIR_EFFECT_READ_MEMORY | NYIR_EFFECT_MAY_TRAP},
    {"rt_load64_idx",
     NYIR_EFFECT_CALL | NYIR_EFFECT_READ_MEMORY | NYIR_EFFECT_MAY_TRAP},
    {"rt_load64_h",
     NYIR_EFFECT_CALL | NYIR_EFFECT_READ_MEMORY | NYIR_EFFECT_MAY_TRAP},
    {"rt_store8_idx",
     NYIR_EFFECT_CALL | NYIR_EFFECT_WRITE_MEMORY | NYIR_EFFECT_MAY_TRAP},
    {"rt_store16_idx",
     NYIR_EFFECT_CALL | NYIR_EFFECT_WRITE_MEMORY | NYIR_EFFECT_MAY_TRAP},
    {"rt_store32_idx",
     NYIR_EFFECT_CALL | NYIR_EFFECT_WRITE_MEMORY | NYIR_EFFECT_MAY_TRAP},
    {"rt_store64_idx",
     NYIR_EFFECT_CALL | NYIR_EFFECT_WRITE_MEMORY | NYIR_EFFECT_MAY_TRAP},
    {"rt_store64_h", NYIR_EFFECT_CALL | NYIR_EFFECT_READ_MEMORY |
                         NYIR_EFFECT_WRITE_MEMORY | NYIR_EFFECT_MAY_TRAP},
    /*
     * Raw copy/fill bridges access objects reachable from their explicit
     * pointer operands. The effect mask does not preserve those operands as
     * alias sets or prove their runtime length, so every WRITE_MEMORY
     * consumer must retain a global barrier.
     */
    {"rt_memcpy_raw", NYIR_EFFECT_CALL | NYIR_EFFECT_READ_MEMORY |
                          NYIR_EFFECT_WRITE_MEMORY | NYIR_EFFECT_MAY_TRAP},
    {"rt_memset_raw",
     NYIR_EFFECT_CALL | NYIR_EFFECT_WRITE_MEMORY | NYIR_EFFECT_MAY_TRAP},
    {"rt_memcmp_raw",
     NYIR_EFFECT_CALL | NYIR_EFFECT_READ_MEMORY | NYIR_EFFECT_MAY_TRAP},
    /*
     * Type probing consults heap registries and may inspect candidate object
     * headers reached through its value argument.
     */
    {"rt_native_is_int", NYIR_EFFECT_CALL | NYIR_EFFECT_READ_MEMORY},
    {"rt_value_to_f64", NYIR_EFFECT_CALL},
    {"rt_dict_get_raw",
     NYIR_EFFECT_CALL | NYIR_EFFECT_READ_MEMORY | NYIR_EFFECT_MAY_TRAP},
    {"rt_dict_has_raw",
     NYIR_EFFECT_CALL | NYIR_EFFECT_READ_MEMORY | NYIR_EFFECT_MAY_TRAP},
    {"rt_dict_len_raw",
     NYIR_EFFECT_CALL | NYIR_EFFECT_READ_MEMORY | NYIR_EFFECT_MAY_TRAP},
    {"rt_any_to_f64",
     NYIR_EFFECT_CALL | NYIR_EFFECT_READ_MEMORY | NYIR_EFFECT_MAY_TRAP},
    {"rt_bigint_cmp_raw",
     NYIR_EFFECT_CALL | NYIR_EFFECT_READ_MEMORY | NYIR_EFFECT_MAY_TRAP},
    {"rt_bigint_bitlen_raw",
     NYIR_EFFECT_CALL | NYIR_EFFECT_READ_MEMORY | NYIR_EFFECT_MAY_TRAP},
    {"rt_bigint_to_i64_raw",
     NYIR_EFFECT_CALL | NYIR_EFFECT_READ_MEMORY | NYIR_EFFECT_MAY_TRAP},
    {"rt_bigfloat_cmp_raw",
     NYIR_EFFECT_CALL | NYIR_EFFECT_READ_MEMORY | NYIR_EFFECT_MAY_TRAP},
    {"rt_bigfloat_precision_raw",
     NYIR_EFFECT_CALL | NYIR_EFFECT_READ_MEMORY | NYIR_EFFECT_MAY_TRAP},
    {"rt_bigfloat_to_f64_raw",
     NYIR_EFFECT_CALL | NYIR_EFFECT_READ_MEMORY | NYIR_EFFECT_MAY_TRAP},

    /*
     * Allocation-only helpers whose inputs are raw scalars.
     */
    {"rt_tbuf_new_raw", NYIR_EFFECT_CALL | NYIR_EFFECT_ALLOCATION},
    {"rt_dict_new_raw", NYIR_EFFECT_CALL | NYIR_EFFECT_ALLOCATION},
    {"rt_i64_to_cstr_raw", NYIR_EFFECT_CALL | NYIR_EFFECT_ALLOCATION},
    {"rt_bigint_from_i64_raw", NYIR_EFFECT_CALL | NYIR_EFFECT_ALLOCATION},

    /*
     * Allocation plus reads from input storage.
     */
    {"rt_cstr_concat", NYIR_EFFECT_CALL | NYIR_EFFECT_READ_MEMORY |
                           NYIR_EFFECT_ALLOCATION | NYIR_EFFECT_MAY_TRAP},
    {"rt_cstr_repeat", NYIR_EFFECT_CALL | NYIR_EFFECT_READ_MEMORY |
                           NYIR_EFFECT_ALLOCATION | NYIR_EFFECT_MAY_TRAP},
    {"rt_cstr_builder_new", NYIR_EFFECT_CALL | NYIR_EFFECT_READ_MEMORY |
                                NYIR_EFFECT_ALLOCATION | NYIR_EFFECT_MAY_TRAP},
    /*
     * Converts a validated raw C string into a managed string. It reads the
     * caller-provided bytes and may allocate, but does not write caller
     * memory or publish unrelated side effects.
     */
    {"rt_cstr_to_str", NYIR_EFFECT_CALL | NYIR_EFFECT_READ_MEMORY |
                           NYIR_EFFECT_ALLOCATION | NYIR_EFFECT_MAY_TRAP},
    {"rt_cstr_replace", NYIR_EFFECT_CALL | NYIR_EFFECT_READ_MEMORY |
                            NYIR_EFFECT_ALLOCATION | NYIR_EFFECT_MAY_TRAP},
    {"rt_any_to_cstr", NYIR_EFFECT_CALL | NYIR_EFFECT_READ_MEMORY |
                           NYIR_EFFECT_ALLOCATION | NYIR_EFFECT_MAY_TRAP},
    {"rt_bigfloat_from_value_raw", NYIR_EFFECT_CALL | NYIR_EFFECT_READ_MEMORY |
                                       NYIR_EFFECT_ALLOCATION |
                                       NYIR_EFFECT_MAY_TRAP},
    {"rt_bigfloat_pow_int_raw", NYIR_EFFECT_CALL | NYIR_EFFECT_READ_MEMORY |
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
    {"rt_tbuf_append_raw", NYIR_EFFECT_CALL | NYIR_EFFECT_READ_MEMORY |
                               NYIR_EFFECT_WRITE_MEMORY |
                               NYIR_EFFECT_ALLOCATION | NYIR_EFFECT_MAY_TRAP},
    {"rt_tbuf_append_i64_raw",
     NYIR_EFFECT_CALL | NYIR_EFFECT_READ_MEMORY | NYIR_EFFECT_WRITE_MEMORY |
         NYIR_EFFECT_ALLOCATION | NYIR_EFFECT_MAY_TRAP},
    {"rt_tbuf_pop_raw", NYIR_EFFECT_CALL | NYIR_EFFECT_READ_MEMORY |
                            NYIR_EFFECT_WRITE_MEMORY | NYIR_EFFECT_MAY_TRAP},
    {"rt_tbuf_set_tagged", NYIR_EFFECT_CALL | NYIR_EFFECT_READ_MEMORY |
                               NYIR_EFFECT_WRITE_MEMORY | NYIR_EFFECT_MAY_TRAP},
    {"rt_tbuf_swap", NYIR_EFFECT_CALL | NYIR_EFFECT_READ_MEMORY |
                         NYIR_EFFECT_WRITE_MEMORY | NYIR_EFFECT_MAY_TRAP},
    /*
     * RC helpers share a synchronized process-wide ownership table. Retain
     * may allocate a table entry; release/drop may free the referent.
     */
    {"rt_retain_owned", NYIR_EFFECT_CALL | NYIR_EFFECT_READ_MEMORY |
                            NYIR_EFFECT_WRITE_MEMORY | NYIR_EFFECT_ALLOCATION},
    {"rt_rc_count", NYIR_EFFECT_CALL | NYIR_EFFECT_READ_MEMORY},
    {"rt_release_owned", NYIR_EFFECT_CALL | NYIR_EFFECT_READ_MEMORY |
                             NYIR_EFFECT_WRITE_MEMORY | NYIR_EFFECT_MAY_TRAP},
    {"rt_drop_owned", NYIR_EFFECT_CALL | NYIR_EFFECT_READ_MEMORY |
                          NYIR_EFFECT_WRITE_MEMORY | NYIR_EFFECT_MAY_TRAP},
    {"rt_drop_owned_slot", NYIR_EFFECT_CALL | NYIR_EFFECT_READ_MEMORY |
                               NYIR_EFFECT_WRITE_MEMORY | NYIR_EFFECT_MAY_TRAP},
    {"rt_dict_set_raw", NYIR_EFFECT_CALL | NYIR_EFFECT_READ_MEMORY |
                            NYIR_EFFECT_WRITE_MEMORY | NYIR_EFFECT_ALLOCATION |
                            NYIR_EFFECT_MAY_TRAP},
    {"rt_dict_get_i64_raw",
     NYIR_EFFECT_CALL | NYIR_EFFECT_READ_MEMORY | NYIR_EFFECT_MAY_TRAP},
    {"rt_dict_set_i64_raw", NYIR_EFFECT_CALL | NYIR_EFFECT_READ_MEMORY |
                                NYIR_EFFECT_WRITE_MEMORY |
                                NYIR_EFFECT_ALLOCATION | NYIR_EFFECT_MAY_TRAP},
    {"rt_dict_merge_raw", NYIR_EFFECT_CALL | NYIR_EFFECT_READ_MEMORY |
                              NYIR_EFFECT_WRITE_MEMORY |
                              NYIR_EFFECT_ALLOCATION | NYIR_EFFECT_MAY_TRAP},
    {"rt_dict_keys_raw", NYIR_EFFECT_CALL | NYIR_EFFECT_READ_MEMORY |
                             NYIR_EFFECT_ALLOCATION | NYIR_EFFECT_MAY_TRAP},
    {"rt_dict_values_raw", NYIR_EFFECT_CALL | NYIR_EFFECT_READ_MEMORY |
                               NYIR_EFFECT_ALLOCATION | NYIR_EFFECT_MAY_TRAP},
    {"rt_dict_items_raw", NYIR_EFFECT_CALL | NYIR_EFFECT_READ_MEMORY |
                              NYIR_EFFECT_ALLOCATION | NYIR_EFFECT_MAY_TRAP},
    {"rt_dict_get_str_raw",
     NYIR_EFFECT_CALL | NYIR_EFFECT_READ_MEMORY | NYIR_EFFECT_MAY_TRAP},
    {"rt_dict_set_str_raw", NYIR_EFFECT_CALL | NYIR_EFFECT_READ_MEMORY |
                                NYIR_EFFECT_WRITE_MEMORY |
                                NYIR_EFFECT_ALLOCATION | NYIR_EFFECT_MAY_TRAP},
    {"rt_dict_has_str_raw",
     NYIR_EFFECT_CALL | NYIR_EFFECT_READ_MEMORY | NYIR_EFFECT_MAY_TRAP},
    {"rt_dict_delete_str_raw", NYIR_EFFECT_CALL | NYIR_EFFECT_READ_MEMORY |
                                   NYIR_EFFECT_WRITE_MEMORY |
                                   NYIR_EFFECT_MAY_TRAP},
    {"rt_dict_delete_raw", NYIR_EFFECT_CALL | NYIR_EFFECT_READ_MEMORY |
                               NYIR_EFFECT_WRITE_MEMORY | NYIR_EFFECT_MAY_TRAP},
    {"rt_any_collection_remove_raw",
     NYIR_EFFECT_CALL | NYIR_EFFECT_READ_MEMORY | NYIR_EFFECT_WRITE_MEMORY |
         NYIR_EFFECT_MAY_TRAP},
    {"rt_cstr_builder_append",
     NYIR_EFFECT_CALL | NYIR_EFFECT_READ_MEMORY | NYIR_EFFECT_WRITE_MEMORY |
         NYIR_EFFECT_ALLOCATION | NYIR_EFFECT_MAY_TRAP},
    {"rt_cstr_builder_append_known",
     NYIR_EFFECT_CALL | NYIR_EFFECT_READ_MEMORY | NYIR_EFFECT_WRITE_MEMORY |
         NYIR_EFFECT_ALLOCATION | NYIR_EFFECT_MAY_TRAP},
    {"rt_cstr_builder_finalize", NYIR_EFFECT_CALL | NYIR_EFFECT_READ_MEMORY |
                                     NYIR_EFFECT_WRITE_MEMORY |
                                     NYIR_EFFECT_MAY_TRAP},
    /*
     * Tag tests update private runtime lookup caches in some cases.
     */
    {"rt_native_has_tag", NYIR_EFFECT_CALL | NYIR_EFFECT_READ_MEMORY |
                              NYIR_EFFECT_WRITE_MEMORY | NYIR_EFFECT_MAY_TRAP},
    {"rt_value_tag", NYIR_EFFECT_CALL | NYIR_EFFECT_READ_MEMORY |
                         NYIR_EFFECT_WRITE_MEMORY | NYIR_EFFECT_MAY_TRAP},

    /*
     * Dedicated externally observable effect classes keep these calls
     * conservative without collapsing unrelated memory into UNKNOWN.
     */
    {"rt_sin_f64", NYIR_EFFECT_CALL | NYIR_EFFECT_FENV},
    {"rt_cos_f64", NYIR_EFFECT_CALL | NYIR_EFFECT_FENV},
    {"rt_fmod_f64", NYIR_EFFECT_CALL | NYIR_EFFECT_FENV},
    {"rt_assert_cstr", NYIR_EFFECT_CALL | NYIR_EFFECT_READ_MEMORY |
                           NYIR_EFFECT_MAY_TRAP | NYIR_EFFECT_IO},
    {"rt_thread_spawn_raw",
     NYIR_EFFECT_CALL | NYIR_EFFECT_ALLOCATION | NYIR_EFFECT_THREAD},
    {"rt_print_cstr", NYIR_EFFECT_CALL | NYIR_EFFECT_READ_MEMORY |
                          NYIR_EFFECT_MAY_TRAP | NYIR_EFFECT_IO},
    {"rt_print_i64_raw", NYIR_EFFECT_CALL | NYIR_EFFECT_IO},
    {"rt_print_f64_raw", NYIR_EFFECT_CALL | NYIR_EFFECT_IO | NYIR_EFFECT_FENV},
    {"rt_print_newline", NYIR_EFFECT_CALL | NYIR_EFFECT_IO},
};

unsigned nyr_call_effect_summary(const nyr_inst_t *inst) {
  if (!inst || inst->op != NYIR_CALL)
    return NYIR_EFFECT_NONE;
  const char *name = inst->symbol;
  if (!name || !name[0])
    return NYIR_EFFECT_CALL | NYIR_EFFECT_FFI | NYIR_EFFECT_UNKNOWN_SIDE_EFFECT;
  for (size_t i = 0;
       i < sizeof(nyr_runtime_effects) / sizeof(nyr_runtime_effects[0]); ++i)
    if (strcmp(name, nyr_runtime_effects[i].name) == 0)
      return nyr_runtime_effects[i].effects;
  return NYIR_EFFECT_CALL | NYIR_EFFECT_FFI | NYIR_EFFECT_UNKNOWN_SIDE_EFFECT;
}

unsigned nyr_effective_effects(const nyr_inst_t *inst) {
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
    return nyr_call_effect_summary(inst);
  }
  return inst->effects | nyr_inst_effects(inst);
}

/*
 * Effect / alias summary: collapse per-instruction effects into one mask.
 */
bool nyr_effect_summary(const nyr_func_t *f, unsigned *out_mask,
                         size_t *out_reads, size_t *out_writes,
                         size_t *out_calls) {
  if (!f || !out_mask)
    return false;
  unsigned mask = 0;
  size_t reads = 0, writes = 0, calls = 0;
  for (size_t i = 0; i < f->len; ++i) {
    unsigned e = nyr_effective_effects(&f->data[i]);
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
