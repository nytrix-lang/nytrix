/*
 * Unified Multi-Tier SROA Engine:
 * Combines points-to promotion, escape analysis, dead store elimination,
 * and scalar/aggregate replacement into a single cohesive pipeline with
 * shared local escape analysis and minimal graph rescanning.
 */
#include "code/backend/passes/util.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool nyr_sroa_unified(nyr_func_t *f);
bool nyr_sroa_scalar(nyr_func_t *f);
bool nyr_escape_sroa(nyr_func_t *f);

/*
 * `pt` holds a local slot, -1 for unknown provenance, or this null marker.
 */
#define SROA_PT_NULL_CONST (-2)

typedef struct {
  int64_t offset;
  unsigned width;
  size_t slot;
  bool valid;
} sroa_stack_field_t;

static unsigned sroa_memory_width(const nyr_inst_t *in) {
  return in->flags & NYIR_INST_F_MEM_BYTE  ? 1
         : in->flags & NYIR_INST_F_MEM_U16 ? 2
         : in->flags & NYIR_INST_F_MEM_I32 ? 4
                                           : 8;
}

static nyr_inst_t sroa_raw_inst(nyr_op_t op, int dst, int a, int b, int64_t imm,
                                nyr_debug_loc_t debug) {
  return (nyr_inst_t){.op = op,
                      .dst = dst,
                      .a = a,
                      .b = b,
                      .c = -1,
                      .d = -1,
                      .e = -1,
                      .f = -1,
                      .imm = imm,
                      .scalar_class = NYIR_SCALAR_I64_RAW,
                      .debug = debug};
}

/*
 * Mirror integer fields of a nonescaping C stack object into local slots.
 * Keep the original stores: overlapping wide stores may initialize padding
 * and other fields. A field is eligible only when every overlapping write
 * covers it completely and a covering store dominates every read. Thus the
 * slots preserve partial-width loads without inventing initialized memory.
 */
static bool sroa_private_stack_fields(nyr_func_t *f, bool *changed) {
  *changed = false;
  if (f->next_value <= 0)
    return true;
  bool has_alloca = false, has_load = false;
  for (size_t i = 0; i < f->len; ++i) {
    has_alloca |= f->data[i].op == NYIR_ALLOCA;
    has_load |= f->data[i].op == NYIR_LOAD_I64;
  }
  if (!has_alloca || !has_load)
    return true;
  nyr_use_def_t uses = {0};
  nyr_cfg_t cfg = {0};
  size_t nv = (size_t)f->next_value;
  bool *known = calloc(nv, sizeof(*known));
  int64_t *constants = calloc(nv, sizeof(*constants));
  int64_t *offsets = malloc(nv * sizeof(*offsets));
  bool *derived = calloc(nv, sizeof(*derived));
  bool *var_derived = calloc(nv, sizeof(*var_derived));
  int *queue = malloc(nv * sizeof(*queue));
  bool *memory = calloc(f->len, sizeof(*memory));
  sroa_stack_field_t *fields = calloc(f->len, sizeof(*fields));
  if (!known || !constants || !offsets || !derived || !var_derived || !queue ||
      !memory || !fields || !nir_collect_consts(f, known, constants) ||
      !nyr_build_use_def(f, &uses) || !nyr_cfg_build(f, &cfg)) {
    free(known);
    free(constants);
    free(offsets);
    free(derived);
    free(var_derived);
    free(queue);
    free(memory);
    free(fields);
    nyr_use_def_free(&uses);
    nyr_cfg_free(&cfg);
    return false;
  }
  bool ok = true;
  for (size_t alloc = 0; ok && alloc < f->len; ++alloc) {
    const nyr_inst_t *allocation = &f->data[alloc];
    if (allocation->op != NYIR_ALLOCA || allocation->dst < 0 ||
        allocation->imm <= 0)
      continue;
    memset(derived, 0, nv * sizeof(*derived));
    memset(var_derived, 0, nv * sizeof(*var_derived));
    memset(memory, 0, f->len * sizeof(*memory));
    size_t tail = 1, head = 0, field_count = 0;
    queue[0] = allocation->dst;
    derived[allocation->dst] = true;
    offsets[allocation->dst] = 0;
    bool safe = true;
    while (safe && head < tail) {
      int value = queue[head++];
      bool is_var = var_derived[value];
      for (size_t u = uses.offsets[value]; safe && u < uses.offsets[value + 1];
           ++u) {
        size_t pc = uses.users[u];
        const nyr_inst_t *in = &f->data[pc];
        if (in->op == NYIR_COPY && in->a == value && in->dst >= 0) {
          if (!derived[in->dst]) {
            derived[in->dst] = true;
            var_derived[in->dst] = is_var;
            offsets[in->dst] = offsets[value];
            queue[tail++] = in->dst;
          } else if (is_var && !var_derived[in->dst]) {
            safe = false;
            break;
          }
        } else if ((in->op == NYIR_ADD_I64 || in->op == NYIR_SUB_I64) &&
                   in->dst >= 0 &&
                   (in->a == value ||
                    (in->op == NYIR_ADD_I64 && in->b == value))) {
          int other = in->a == value ? in->b : in->a;
          if (!is_var && other >= 0 && known[other]) {
            int64_t delta = constants[other];
            __int128 offset =
                (__int128)offsets[value] +
                (in->op == NYIR_SUB_I64 ? -(__int128)delta : delta);
            if (offset < 0 || offset > allocation->imm) {
              safe = false;
              break;
            }
            if (!derived[in->dst]) {
              derived[in->dst] = true;
              var_derived[in->dst] = false;
              offsets[in->dst] = (int64_t)offset;
              queue[tail++] = in->dst;
            } else if (var_derived[in->dst] ||
                       offsets[in->dst] != (int64_t)offset) {
              safe = false;
              break;
            }
          } else {
            /*
             * Variable-offset pointer into this alloca: legal as long as it
             * only feeds read-only loads (never a store or escaping use),
             * since original stores remain in place.
             */
            if (!derived[in->dst]) {
              derived[in->dst] = true;
              var_derived[in->dst] = true;
              offsets[in->dst] = 0;
              queue[tail++] = in->dst;
            } else if (!var_derived[in->dst]) {
              safe = false;
              break;
            }
          }
        } else if ((in->op == NYIR_VEC4_LOAD_I64 ||
                    in->op == NYIR_VEC8_LOAD_I64 ||
                    in->op == NYIR_VEC4_LOAD_F64 ||
                    in->op == NYIR_VEC8_LOAD_F32) &&
                   in->a == value) {
          /*
           * Read-only vector load: original stores stay intact.
           */
          continue;
        } else if (is_var && in->op == NYIR_LOAD_I64 && in->a == value &&
                   !(in->effects & NYIR_EFFECT_VOLATILE) &&
                   !(in->flags & NYIR_INST_F_MEM_VOLATILE)) {
          /*
           * Read-only variable-index scalar load: original stores stay.
           */
          continue;
        } else if (!is_var &&
                   (in->op == NYIR_LOAD_I64 || in->op == NYIR_STORE_I64) &&
                   in->a == value && in->imm == 0 &&
                   !(nyr_effective_effects(in) &
                     ~(NYIR_EFFECT_READ_MEMORY | NYIR_EFFECT_WRITE_MEMORY |
                       NYIR_EFFECT_MAY_TRAP)) &&
                   !(in->effects & NYIR_EFFECT_VOLATILE) &&
                   !(in->flags & NYIR_INST_F_MEM_VOLATILE) &&
                   (f->untagged_values
                        ? (in->scalar_class == NYIR_SCALAR_UNKNOWN ||
                           in->scalar_class == NYIR_SCALAR_I64_RAW)
                        : (in->scalar_class == NYIR_SCALAR_I64_RAW)) &&
                   (in->op != NYIR_STORE_I64 || in->c != value)) {
          unsigned width = sroa_memory_width(in);
          if (offsets[value] > allocation->imm - (int64_t)width) {
            safe = false;
            break;
          }
          if (memory[pc])
            continue;
          memory[pc] = true;
          if (in->op == NYIR_LOAD_I64) {
            size_t field = 0;
            while (field < field_count &&
                   (fields[field].offset != offsets[value] ||
                    fields[field].width != width))
              ++field;
            if (field == field_count)
              fields[field_count++] = (sroa_stack_field_t){
                  .offset = offsets[value], .width = width, .valid = true};
          }
        } else {
          safe = false;
        }
      }
    }
    if (!safe || !field_count)
      continue;
    size_t extra = 0, eligible = 0;
    for (size_t field = 0; field < field_count; ++field) {
      sroa_stack_field_t *part = &fields[field];
      for (size_t pc = 0; part->valid && pc < f->len; ++pc) {
        const nyr_inst_t *in = &f->data[pc];
        if (!memory[pc])
          continue;
        int64_t start = offsets[in->a];
        unsigned width = sroa_memory_width(in);
        if (in->op == NYIR_STORE_I64 && start < part->offset + part->width &&
            part->offset < start + width &&
            (start > part->offset ||
             start + width < part->offset + part->width))
          part->valid = false;
        if (in->op != NYIR_LOAD_I64 || start != part->offset ||
            width != part->width)
          continue;
        bool initialized = false;
        for (size_t st = 0; !initialized && st < f->len; ++st) {
          const nyr_inst_t *store = &f->data[st];
          if (!memory[st] || store->op != NYIR_STORE_I64 ||
              offsets[store->a] > part->offset ||
              offsets[store->a] + sroa_memory_width(store) <
                  part->offset + part->width)
            continue;
          size_t sb = cfg.inst_block[st], lb = cfg.inst_block[pc];
          initialized = sb == lb ? st < pc : nyr_cfg_dominates(&cfg, sb, lb);
        }
        if (!initialized)
          part->valid = false;
      }
      if (part->valid) {
        part->slot = nyr_local_slot_count(f) + eligible++;
        for (size_t pc = 0; pc < f->len; ++pc)
          if (memory[pc] && f->data[pc].op == NYIR_STORE_I64 &&
              offsets[f->data[pc].a] <= part->offset &&
              offsets[f->data[pc].a] + sroa_memory_width(&f->data[pc]) >=
                  part->offset + part->width)
            extra += 6;
      }
    }
    if (!eligible)
      continue;
    if (extra > SIZE_MAX - f->len ||
        extra > (size_t)(INT_MAX - f->next_value)) {
      ok = false;
      break;
    }
    size_t capacity = f->len + extra;
    nyr_inst_t *output = calloc(capacity, sizeof(*output));
    if (!output) {
      ok = false;
      break;
    }
    size_t length = 0;
    int next_value = f->next_value;
    for (size_t pc = 0; pc < f->len; ++pc) {
      nyr_inst_t in = f->data[pc];
      if (memory[pc]) {
        for (size_t field = 0; field < field_count; ++field) {
          const sroa_stack_field_t *part = &fields[field];
          if (!part->valid)
            continue;
          if (in.op == NYIR_LOAD_I64 && offsets[in.a] == part->offset &&
              sroa_memory_width(&in) == part->width) {
            in = sroa_raw_inst(NYIR_LOAD_LOCAL, in.dst, -1, -1,
                               (int64_t)part->slot, in.debug);
            break;
          }
          if (in.op != NYIR_STORE_I64 || offsets[in.a] > part->offset ||
              offsets[in.a] + sroa_memory_width(&in) <
                  part->offset + part->width)
            continue;
          int source = in.c;
          unsigned shift = (unsigned)(part->offset - offsets[in.a]) * 8;
          if (shift) {
            int amount = next_value++;
            output[length++] =
                sroa_raw_inst(NYIR_CONST_I64, amount, -1, -1, shift, in.debug);
            int shifted = next_value++;
            output[length++] = sroa_raw_inst(NYIR_SAR_I64, shifted, source,
                                             amount, 0, in.debug);
            source = shifted;
          }
          if (part->width == 4) {
            int amount = next_value++, left = next_value++,
                signed_value = next_value++;
            output[length++] =
                sroa_raw_inst(NYIR_CONST_I64, amount, -1, -1, 32, in.debug);
            output[length++] =
                sroa_raw_inst(NYIR_SHL_I64, left, source, amount, 0, in.debug);
            output[length++] = sroa_raw_inst(NYIR_SAR_I64, signed_value, left,
                                             amount, 0, in.debug);
            source = signed_value;
          } else if (part->width < 4) {
            int mask = next_value++, narrowed = next_value++;
            output[length++] =
                sroa_raw_inst(NYIR_CONST_I64, mask, -1, -1,
                              part->width == 1 ? 255 : 65535, in.debug);
            output[length++] = sroa_raw_inst(NYIR_AND_I64, narrowed, source,
                                             mask, 0, in.debug);
            source = narrowed;
          }
          output[length++] = sroa_raw_inst(NYIR_STORE_LOCAL, -1, source, -1,
                                           (int64_t)part->slot, in.debug);
        }
      }
      output[length++] = in;
    }
    free(f->data);
    f->data = output;
    f->len = length;
    f->cap = capacity;
    f->next_value = next_value;
    *changed = true;
    nyr_refresh_metadata(f);
    /*
     * The use graph and CFG describe the old instruction array.
     */
    break;
  }
  free(known);
  free(constants);
  free(offsets);
  free(derived);
  free(var_derived);
  free(queue);
  free(memory);
  free(fields);
  nyr_use_def_free(&uses);
  nyr_cfg_free(&cfg);
  return ok;
}

bool nyr_sroa_unified(nyr_func_t *f) {
  if (!f || f->len == 0)
    return true;
  bool any_promoted = false;
  for (int round = 0; round < 8; ++round) {
    bool promoted_stack = false;
    if (!sroa_private_stack_fields(f, &promoted_stack))
      return false;
    if (!promoted_stack)
      break;
    any_promoted = true;
  }
  if (any_promoted && !nyr_mem2reg(f))
    return false;

  size_t slot_count = nyr_local_slot_count(f);
  if (!slot_count)
    return true;

  /*
   * Tier 1: Local Points-To Promotion.
   * ADDR_LOCAL v -> local L. When LOAD_I64/STORE_I64 of that pointer
   * and L is never passed to CALL, rewrite to LOAD_LOCAL/STORE_LOCAL.
   */
  if (f->next_value > 0) {
    size_t nv = (size_t)f->next_value;
    int *pt = calloc(nv, sizeof(int));
    /*
     * Escape provenance (mark_pt) is tracked separately from rewrite
     * provenance (pt): a derived pointer exposes its base storage at any
     * offset, so marking must follow arithmetic and merges aggressively,
     * while rewriting must only fire on exact (zero-offset) derivations.
     * Over-marking costs optimization; under-marking miscompiles.
     */
    int *mark_pt = calloc(nv, sizeof(int));
    bool *pt_escaped = calloc(slot_count, sizeof(bool));
    if (!pt || !mark_pt || !pt_escaped) {
      free(pt);
      free(mark_pt);
      free(pt_escaped);
    } else {
      for (size_t i = 0; i < nv; ++i) {
        pt[i] = -1;
        mark_pt[i] = -1;
      }
      bool null_consts_indexed = false;
      for (size_t i = 0; i < f->len; ++i) {
        const nyr_inst_t *in = &f->data[i];
        if (in->op == NYIR_ADDR_LOCAL && in->dst >= 0 && in->imm >= 0) {
          pt[in->dst] = (int)in->imm;
          mark_pt[in->dst] = (int)in->imm;
        } else if (in->op == NYIR_COPY && in->dst >= 0 && in->a >= 0 &&
                   (size_t)in->a < nv) {
          if (pt[in->a] >= 0)
            pt[in->dst] = pt[in->a];
          if (mark_pt[in->a] >= 0)
            mark_pt[in->dst] = mark_pt[in->a];
        } else if (in->op == NYIR_PHI && in->dst >= 0) {
          /*
           * A merged address may alias any mapped source; unmapped
           * (unknown) sources are conservatively assumed to alias them
           * too, except for null constants, which cannot address a
           * frame slot.  Rewrite provenance requires unanimity over
           * mapped sources with no unknown ones; escape marking takes
           * every mapped source whenever the merge is not provably
           * single-sourced.
           */
          int merged = -1;
          bool split = false;
          bool unknown = false;
          for (size_t k = 0; k < in->phi_incoming_len; ++k) {
            int vv = in->phi_incoming[k].value;
            if (vv < 0 || (size_t)vv >= nv) {
              unknown = true;
              continue;
            }
            if (pt[vv] < 0) {
              /*
               * PHI inputs can be defined later in linear order. Build this
               * index lazily: PHIs with only tracked inputs need no scan.
               */
              if (!null_consts_indexed) {
                for (size_t j = 0; j < f->len; ++j) {
                  const nyr_inst_t *candidate = &f->data[j];
                  if (candidate->op == NYIR_CONST_I64 && candidate->imm == 0 &&
                      candidate->dst >= 0 && (size_t)candidate->dst < nv)
                    pt[candidate->dst] = SROA_PT_NULL_CONST;
                }
                null_consts_indexed = true;
              }
              if (pt[vv] != SROA_PT_NULL_CONST)
                unknown = true;
              continue;
            }
            if (merged < 0) {
              merged = pt[vv];
            } else if (merged != pt[vv]) {
              split = true;
              break;
            }
          }
          if (!split && !unknown && merged >= 0) {
            pt[in->dst] = merged;
            mark_pt[in->dst] = merged;
          } else if (split || unknown) {
            for (size_t k = 0; k < in->phi_incoming_len; ++k) {
              int vv = in->phi_incoming[k].value;
              if (vv >= 0 && (size_t)vv < nv && mark_pt[vv] >= 0 &&
                  (size_t)mark_pt[vv] < slot_count)
                pt_escaped[mark_pt[vv]] = true;
            }
          }
        } else if ((in->op == NYIR_ADD_I64 || in->op == NYIR_SUB_I64) &&
                   in->dst >= 0 && in->a >= 0 && (size_t)in->a < nv &&
                   mark_pt[in->a] >= 0) {
          /*
           * Pointer arithmetic (including nonzero offsets) preserves
           * the base provenance for escape purposes only; pt[] is
           * deliberately untouched so rewriting never drops an offset.
           */
          mark_pt[in->dst] = mark_pt[in->a];
          bool second_tracked = in->b >= 0 && (size_t)in->b < nv &&
                                mark_pt[in->b] >= 0 &&
                                mark_pt[in->b] != mark_pt[in->a];
          if (second_tracked && (size_t)mark_pt[in->a] < slot_count &&
              (size_t)mark_pt[in->b] < slot_count) {
            /*
             * Sum of two differently-tracked pointers: the result may
             * alias either source.  Mark both now; the marking pass
             * below cannot see through the value.
             */
            pt_escaped[mark_pt[in->a]] = true;
            pt_escaped[mark_pt[in->b]] = true;
          }
        }
      }
      {
        for (size_t i = 0; i < f->len; ++i) {
          const nyr_inst_t *in = &f->data[i];
          if (in->op == NYIR_CALL) {
            int args[16];
            int argc = 0;
            if (!nyr_call_args(in, f->next_value, args, 16, &argc, NULL, 0))
              continue;
            for (int a = 0; a < argc; ++a) {
              if (args[a] >= 0 && (size_t)args[a] < nv &&
                  mark_pt[args[a]] >= 0 &&
                  (size_t)mark_pt[args[a]] < slot_count)
                pt_escaped[mark_pt[args[a]]] = true;
            }
          } else if (in->op == NYIR_RET && in->a >= 0 && (size_t)in->a < nv &&
                     mark_pt[in->a] >= 0 &&
                     (size_t)mark_pt[in->a] < slot_count) {
            /*
             * A returned derived pointer outlives the frame; the caller
             * may write through it, so the source slot is escaped even
             * when no call in this body observes it.
             */
            pt_escaped[mark_pt[in->a]] = true;
          } else if (in->op == NYIR_STORE_I64 && in->c >= 0 &&
                     (size_t)in->c < nv && mark_pt[in->c] >= 0 &&
                     (size_t)mark_pt[in->c] < slot_count) {
            /*
             * Storing a derived pointer as the *value* publishes it
             * through memory (unlike the address operand, which is just
             * used).  A later call may read it back and write through
             * it, so the source slot is escaped.
             */
            pt_escaped[mark_pt[in->c]] = true;
          }
        }
        for (size_t i = 0; i < f->len; ++i) {
          nyr_inst_t *in = &f->data[i];
          if (in->op == NYIR_LOAD_I64 && in->a >= 0 && (size_t)in->a < nv &&
              pt[in->a] >= 0 && (size_t)pt[in->a] < slot_count &&
              !pt_escaped[pt[in->a]] && in->dst >= 0) {
            nyr_range_t range = in->range;
            nyr_scalar_class_t scalar_class = in->scalar_class;
            uint8_t ownership = in->semantic_ownership;
            bool mutable_fact = in->semantic_mutable;
            nyr_debug_loc_t debug = in->debug;
            *in = (nyr_inst_t){.op = NYIR_LOAD_LOCAL,
                               .dst = in->dst,
                               .a = -1,
                               .b = -1,
                               .imm = pt[in->a],
                               .scalar_class = scalar_class,
                               .semantic_ownership = ownership,
                               .semantic_mutable = mutable_fact,
                               .alias_class = 0,
                               .debug = debug,
                               .range = range};
          } else if (in->op == NYIR_STORE_I64 && in->a >= 0 &&
                     (size_t)in->a < nv && pt[in->a] >= 0 &&
                     (size_t)pt[in->a] < slot_count && !pt_escaped[pt[in->a]] &&
                     in->c >= 0) {
            uint8_t ownership = in->semantic_ownership;
            bool mutable_fact = in->semantic_mutable;
            nyr_debug_loc_t debug = in->debug;
            *in = (nyr_inst_t){.op = NYIR_STORE_LOCAL,
                               .dst = -1,
                               .a = in->c,
                               .b = -1,
                               .c = -1,
                               .imm = pt[in->a],
                               .semantic_ownership = ownership,
                               .semantic_mutable = mutable_fact,
                               .alias_class = 0,
                               .debug = debug};
          }
        }
        free(pt_escaped);
        free(pt);
        free(mark_pt);
      }
    }
  }

  /*
   * Tier 2: Shared Local-Escape & Access Analysis.
   */
  nyr_local_escape_info_t stk_escape[64] = {0};
  bool stk_loaded[64] = {0}, stk_stored[64] = {0};
  nyr_local_escape_info_t *escape =
      slot_count <= 64 ? stk_escape : calloc(slot_count, sizeof(*escape));
  bool *loaded =
      slot_count <= 64 ? stk_loaded : calloc(slot_count, sizeof(*loaded));
  bool *stored =
      slot_count <= 64 ? stk_stored : calloc(slot_count, sizeof(*stored));
  if (!escape || !loaded || !stored ||
      !nyr_analyze_local_escapes(f, escape, slot_count)) {
    if (slot_count > 64) {
      free(escape);
      free(loaded);
      free(stored);
    }
    return false;
  }

  for (size_t i = 0; i < f->len; ++i) {
    const nyr_inst_t *in = &f->data[i];
    if (in->imm < 0 || (size_t)in->imm >= slot_count)
      continue;
    if (in->op == NYIR_LOAD_LOCAL)
      loaded[in->imm] = true;
    else if (in->op == NYIR_STORE_LOCAL)
      stored[in->imm] = true;
  }

  /*
   * Dead stores to private, unread slots are unobservable.
   */
  for (size_t i = 0; i < f->len; ++i) {
    nyr_inst_t *in = &f->data[i];
    if (in->op == NYIR_STORE_LOCAL && in->imm >= 0 &&
        (size_t)in->imm < slot_count && !escape[in->imm].escapes &&
        !loaded[in->imm])
      nyr_inst_discard(in);
  }

  /*
   * Uninitialized private locals have the language-defined zero value.
   */
  for (size_t i = 0; i < f->len; ++i) {
    nyr_inst_t *in = &f->data[i];
    if (in->op == NYIR_LOAD_LOCAL && in->imm >= 0 &&
        (size_t)in->imm >= f->param_count && (size_t)in->imm < slot_count &&
        !escape[in->imm].escapes && !stored[in->imm]) {
      nir_make_const(in, 0);
    }
  }

  /*
   * Tier 3: Aggregate / Field Store Sinking & Scalar Replacement.
   */
  int stk_def[64], stk_last_store[64];
  int *current_def =
      slot_count <= 64 ? stk_def : malloc(slot_count * sizeof(*current_def));
  int *last_store = slot_count <= 64 ? stk_last_store
                                     : malloc(slot_count * sizeof(*last_store));
  if (!current_def || !last_store) {
    if (slot_count > 64) {
      free(escape);
      free(loaded);
      free(stored);
      free(current_def);
      free(last_store);
    }
    return false;
  }
  for (size_t i = 0; i < slot_count; ++i) {
    current_def[i] = -1;
    last_store[i] = -1;
  }

  bool any_replaced = false;
  for (size_t i = 0; i < f->len; ++i) {
    nyr_inst_t *in = &f->data[i];
    if (in->op == NYIR_LABEL || in->op == NYIR_BR || in->op == NYIR_BR_IF ||
        in->op == NYIR_RET) {
      for (size_t k = 0; k < slot_count; ++k) {
        current_def[k] = -1;
        last_store[k] = -1;
      }
      continue;
    }
    if (in->op == NYIR_CALL) {
      /*
       * A local value may be observed through aliases during a call.  The
       * escape summary only answers whether an address is retained; it does
       * not prove that a call cannot write through an address temporarily.
       * End all block-local forwarding facts at this memory boundary.
       */
      for (size_t k = 0; k < slot_count; ++k) {
        current_def[k] = -1;
        last_store[k] = -1;
      }
      continue;
    }
    if (in->op == NYIR_STORE_LOCAL && in->imm >= 0 &&
        (size_t)in->imm < slot_count && !escape[in->imm].escapes) {
      size_t slot = (size_t)in->imm;
      if (last_store[slot] >= 0) {
        size_t prev = (size_t)last_store[slot];
        nyr_inst_discard(&f->data[prev]);
        any_replaced = true;
      }
      last_store[slot] = (int)i;
      current_def[slot] = in->a;
    } else if (in->op == NYIR_LOAD_LOCAL && in->imm >= 0 &&
               (size_t)in->imm < slot_count && !escape[in->imm].escapes) {
      size_t slot = (size_t)in->imm;
      if (current_def[slot] >= 0 && in->dst >= 0) {
        nir_make_copy(in, current_def[slot]);
        last_store[slot] = -1; /* observed */
        any_replaced = true;
      }
    }
  }

  /*
   * Tier 4: Global Dead Store Cleanup.
   */
  if (any_replaced) {
    bool stk_ll[64] = {0};
    bool *local_loaded =
        slot_count <= 64 ? stk_ll : calloc(slot_count, sizeof(bool));
    if (local_loaded) {
      for (size_t i = 0; i < f->len; ++i)
        if (f->data[i].op == NYIR_LOAD_LOCAL && f->data[i].imm >= 0 &&
            (size_t)f->data[i].imm < slot_count)
          local_loaded[f->data[i].imm] = true;
      for (size_t i = 0; i < f->len; ++i) {
        nyr_inst_t *in = &f->data[i];
        if (in->op != NYIR_STORE_LOCAL || in->imm < 0 ||
            (size_t)in->imm >= slot_count || escape[in->imm].escapes)
          continue;
        /*
         * `local_loaded` is computed after scalar replacement, so a load
         * that was rewritten to a COPY disappears from that scan.  The
         * original `loaded` bitmap is the proof that the slot was observable;
         * dropping its initializing store leaves later CFG paths reading an
         * uninitialized local (notably ALLOCA-backed buffer pointers).
         */
        if (loaded[in->imm] || local_loaded[in->imm])
          continue;
        nyr_inst_discard(in);
      }
      if (slot_count > 64)
        free(local_loaded);
    }
    (void)nyr_compact_if_sparse(f);
  }

  if (slot_count > 64) {
    free(escape);
    free(loaded);
    free(stored);
    free(current_def);
    free(last_store);
  }
  return true;
}

bool nyr_sroa_scalar(nyr_func_t *f) { return nyr_sroa_unified(f); }
