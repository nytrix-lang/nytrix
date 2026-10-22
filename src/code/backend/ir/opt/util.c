/*
 * NYIR opt utilities: shared helpers for pass registration, IR
 * traversal, dead-block removal, and value-replacement bookkeeping.
 *
 * References:
 *  Cytron, Ferrante, Rosen, Wegman, Zadeck — "Efficiently Computing
 *   Static Single Assignment Form and the Control Dependence Graph"
 *   (TOPLAS 1991). §3–5: dominance frontiers, φ-node placement,
 *   renaming pass for SSA construction.
 *  Cooper & Torczon — Engineering a Compiler, 2nd Ed., Ch.8 §8.5.
 */
#include "code/backend/ir/opt/util.h"
#include "base/common.h"
#include "base/compat.h"
#include "code/backend/ir/internal.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

size_t nyr_local_slot_count(const nyr_func_t *f) {
  if (!f)
    return 0;
  size_t max_slot = 0;
  bool have_slot = false;
  for (size_t i = 0; i < f->len; ++i) {
    const nyr_inst_t *in = &f->data[i];
    if ((in->op != NYIR_ADDR_LOCAL && in->op != NYIR_LOAD_LOCAL &&
         in->op != NYIR_STORE_LOCAL) ||
        in->imm < 0)
      continue;
    size_t slot = (size_t)in->imm;
    if (!have_slot || slot > max_slot)
      max_slot = slot;
    have_slot = true;
  }
  return have_slot ? max_slot + 1u : 0u;
}

static int nyr_local_slot_from_value(const nyr_func_t *f, const int *defs,
                                      int value) {
  if (!f || !defs)
    return -1;
  for (unsigned depth = 0; depth < 16 && value >= 0 && value < f->next_value;
       ++depth) {
    int di = defs[value];
    if (di < 0 || (size_t)di >= f->len)
      return -1;
    const nyr_inst_t *def = &f->data[(size_t)di];
    if (def->op == NYIR_ADDR_LOCAL)
      return def->imm >= 0 && def->imm <= INT_MAX ? (int)def->imm : -1;
    if (def->op != NYIR_COPY)
      return -1;
    value = def->a;
  }
  return -1;
}

static void nyr_mark_local_escape(nyr_local_escape_info_t *info,
                                   size_t slot_count, int slot, size_t pc,
                                   bool from_call, bool returned, bool stored) {
  if (!info || slot < 0 || (size_t)slot >= slot_count)
    return;
  nyr_local_escape_info_t *e = &info[slot];
  if (from_call) {
    e->passed_to_call = true;
    /*
     * Only mark as escaping if the call has effects that could capture
     * the argument. The flags ffi_escape, thread_escape, and
     * unknown_call_escape are set by the caller based on effective effects.
     */
    if (e->ffi_escape) {
      e->escapes = true;
    }
    if (e->thread_escape) {
      e->escapes = true;
    }
    if (e->unknown_call_escape) {
      e->escapes = true;
    }
  } else {
    e->address_taken = true;
    e->escapes = true;
  }
  e->returned |= returned;
  e->stored_to_memory |= stored;
  if (returned || stored) {
    e->escapes = true;
  }
  if (e->escapes && e->first_escape_pc == SIZE_MAX)
    e->first_escape_pc = pc;
}

bool nyr_analyze_local_escapes(const nyr_func_t *f,
                                nyr_local_escape_info_t *info,
                                size_t slot_count) {
  if (!f || (!info && slot_count))
    return false;
  for (size_t i = 0; i < slot_count; ++i) {
    info[i] = (nyr_local_escape_info_t){0};
    info[i].first_escape_pc = SIZE_MAX;
  }
  if (!slot_count)
    return true;
  int *defs = nyr_build_defs(f);
  if (f->next_value > 0 && !defs)
    return false;
  for (size_t pc = 0; pc < f->len; ++pc) {
    const nyr_inst_t *in = &f->data[pc];
    if (in->op == NYIR_ADDR_LOCAL && in->imm >= 0 &&
        (size_t)in->imm < slot_count) {
      nyr_local_escape_info_t *e = &info[in->imm];
      e->address_taken = true;
      e->escapes = true;
      if (e->first_escape_pc == SIZE_MAX)
        e->first_escape_pc = pc;
    }

    if (in->op == NYIR_CALL) {
      unsigned effects = nyr_effective_effects(in);
      int args[6] = {in->a, in->b, in->c, in->d, in->e, in->f};
      for (size_t k = 0; k < 6; ++k) {
        int slot = nyr_local_slot_from_value(f, defs, args[k]);
        if (slot >= 0 && (size_t)slot < slot_count) {
          info[slot].ffi_escape |= (effects & NYIR_EFFECT_FFI) != 0;
          info[slot].thread_escape |= (effects & NYIR_EFFECT_THREAD) != 0;
          info[slot].unknown_call_escape |=
              (effects & NYIR_EFFECT_UNKNOWN_SIDE_EFFECT) != 0;
        }
        nyr_mark_local_escape(info, slot_count, slot, pc, true, false, false);
      }
      for (size_t k = 0; k < in->extra_args_len; ++k) {
        int slot = nyr_local_slot_from_value(f, defs, in->extra_args[k]);
        if (slot >= 0 && (size_t)slot < slot_count) {
          info[slot].ffi_escape |= (effects & NYIR_EFFECT_FFI) != 0;
          info[slot].thread_escape |= (effects & NYIR_EFFECT_THREAD) != 0;
          info[slot].unknown_call_escape |=
              (effects & NYIR_EFFECT_UNKNOWN_SIDE_EFFECT) != 0;
        }
        nyr_mark_local_escape(info, slot_count, slot, pc, true, false, false);
      }
    } else if (in->op == NYIR_RET) {
      int slot = nyr_local_slot_from_value(f, defs, in->a);
      nyr_mark_local_escape(info, slot_count, slot, pc, false, true, false);
    } else if (in->op == NYIR_STORE_I64) {
      /*
       * Storing a local address as the value publishes it through memory.
       */
      int slot = nyr_local_slot_from_value(f, defs, in->c);
      nyr_mark_local_escape(info, slot_count, slot, pc, false, false, true);
    }
  }
  free(defs);
  return true;
}

bool nir_collect_consts_kind(const nyr_func_t *f, bool *known, int64_t *value,
                             unsigned char *kind) {
  if (!f || !known || !value || !kind)
    return false;
  for (size_t i = 0; i < f->len; ++i) {
    const nyr_inst_t *in = &f->data[i];
    if (in->dst < 0)
      continue;
    if (in->op == NYIR_CONST_I64) {
      known[in->dst] = true;
      value[in->dst] = in->imm;
      kind[in->dst] = 1;
    } else if (in->op == NYIR_CONST_F64) {
      known[in->dst] = true;
      value[in->dst] = in->imm;
      kind[in->dst] = 2;
    } else if (in->op == NYIR_CONST_F32) {
      known[in->dst] = true;
      value[in->dst] = in->imm;
      kind[in->dst] = 3;
    } else if (in->op == NYIR_COPY && in->a >= 0 && known[in->a]) {
      known[in->dst] = true;
      value[in->dst] = value[in->a];
      kind[in->dst] = kind[in->a];
    } else {
      known[in->dst] = false;
      kind[in->dst] = 0;
    }
  }
  return true;
}

bool nir_collect_consts(const nyr_func_t *f, bool *known, int64_t *value) {
  if (!f || !known || !value)
    return false;
  /*
   * Integer-only view: CONST_F64/F32 immediates are IEEE bit patterns, not
   * integer values.  Treating them as integers let ADD_I64 x+0 / AND -1
   * folds fire on float bits (word-type confusion).  Callers needing float
   * bits must use nir_collect_consts_kind and check kind==2/3.
   */
  for (size_t i = 0; i < f->len; ++i) {
    const nyr_inst_t *in = &f->data[i];
    if (in->op == NYIR_CONST_I64 && in->dst >= 0) {
      known[in->dst] = true;
      value[in->dst] = in->imm;
    } else if (in->op == NYIR_COPY && in->dst >= 0 && in->a >= 0 &&
               known[in->a]) {
      known[in->dst] = true;
      value[in->dst] = value[in->a];
    } else if (in->dst >= 0) {
      known[in->dst] = false;
    }
  }
  return true;
}

void nir_make_copy(nyr_inst_t *in, int src) {
  int dst = in->dst;
  nyr_debug_loc_t debug = in->debug;
  nyr_scalar_class_t scalar_class = in->scalar_class;
  uint8_t semantic_ownership = in->semantic_ownership;
  bool semantic_mutable = in->semantic_mutable;
  /*
   * Range and alias classes describe the old producer and its operands. The
   * helper cannot prove they hold for `src`, so leave both unknown until their
   * analyses recompute facts for the rewritten instruction.
   */
  *in = (nyr_inst_t){.op = NYIR_COPY,
                      .dst = dst,
                      .a = src,
                      .b = -1,
                      .c = -1,
                      .d = -1,
                      .e = -1,
                      .f = -1,
                      .scalar_class = scalar_class,
                      .semantic_ownership = semantic_ownership,
                      .semantic_mutable = semantic_mutable,
                      .debug = debug};
}

void nir_make_const(nyr_inst_t *in, int64_t value) {
  int dst = in->dst;
  nyr_debug_loc_t debug = in->debug;
  /*
   * Preserve the metadata trio alongside range: CSE hashes ownership and
   * consumers read scalar_class off the defining instruction, so folding an
   * ADD of owned/scalar-typed operands into a bare constant silently
   * created a different entity than the unfolded twin.
   */
  nyr_scalar_class_t scalar_class = in->scalar_class;
  uint8_t semantic_ownership = in->semantic_ownership;
  bool semantic_mutable = in->semantic_mutable;
  *in = (nyr_inst_t){
      .op = NYIR_CONST_I64,
      .dst = dst,
      .a = -1,
      .b = -1,
      .c = -1,
      .d = -1,
      .e = -1,
      .f = -1,
      .imm = value,
      .scalar_class = scalar_class,
      .semantic_ownership = semantic_ownership,
      .semantic_mutable = semantic_mutable,
      .debug = debug,
      .range = {.has_min = true, .has_max = true, .min = value, .max = value}};
}

void nir_make_f64_const(nyr_inst_t *in, int64_t bitcast) {
  int dst = in->dst;
  nyr_debug_loc_t debug = in->debug;
  nyr_scalar_class_t scalar_class = in->scalar_class;
  uint8_t semantic_ownership = in->semantic_ownership;
  bool semantic_mutable = in->semantic_mutable;
  *in = (nyr_inst_t){.op = NYIR_CONST_F64,
                      .dst = dst,
                      .a = -1,
                      .b = -1,
                      .c = -1,
                      .d = -1,
                      .e = -1,
                      .f = -1,
                      .imm = bitcast,
                      .scalar_class = scalar_class,
                      .semantic_ownership = semantic_ownership,
                      .semantic_mutable = semantic_mutable,
                      .debug = debug};
}

void nir_make_f32_const(nyr_inst_t *in, int64_t bitcast) {
  int dst = in->dst;
  nyr_debug_loc_t debug = in->debug;
  nyr_scalar_class_t scalar_class = in->scalar_class;
  uint8_t semantic_ownership = in->semantic_ownership;
  bool semantic_mutable = in->semantic_mutable;
  *in = (nyr_inst_t){.op = NYIR_CONST_F32,
                      .dst = dst,
                      .a = -1,
                      .b = -1,
                      .c = -1,
                      .d = -1,
                      .e = -1,
                      .f = -1,
                      .imm = bitcast,
                      .scalar_class = scalar_class,
                      .semantic_ownership = semantic_ownership,
                      .semantic_mutable = semantic_mutable,
                      .debug = debug};
}

void nyr_clear_derived_facts(nyr_inst_t *in) {
  if (!in)
    return;
  in->range = (nyr_range_t){0};
  in->alias_class = 0;
}

bool nir_cmp_same_value(nyr_cmp_t cmp, int64_t *out) {
  if (!out)
    return false;
  switch (cmp) {
  case NYIR_CMP_EQ:
  case NYIR_CMP_LE:
  case NYIR_CMP_GE:
    *out = 1;
    return true;
  case NYIR_CMP_NE:
  case NYIR_CMP_LT:
  case NYIR_CMP_GT:
    *out = 0;
    return true;
  }
  return false;
}

bool nir_cmp_range_fold(nyr_cmp_t cmp, const nyr_range_t *a,
                        const nyr_range_t *b, int64_t *out) {
  if (!a || !b || !out)
    return false;
  switch (cmp) {
  case NYIR_CMP_EQ:
    if ((a->has_max && b->has_min && a->max < b->min) ||
        (b->has_max && a->has_min && b->max < a->min)) {
      *out = 0;
      return true;
    }
    return false;
  case NYIR_CMP_NE:
    if ((a->has_max && b->has_min && a->max < b->min) ||
        (b->has_max && a->has_min && b->max < a->min)) {
      *out = 1;
      return true;
    }
    return false;
  case NYIR_CMP_LT:
    if (a->has_max && b->has_min && a->max < b->min) {
      *out = 1;
      return true;
    }
    if (a->has_min && b->has_max && a->min >= b->max) {
      *out = 0;
      return true;
    }
    return false;
  case NYIR_CMP_LE:
    if (a->has_max && b->has_min && a->max <= b->min) {
      *out = 1;
      return true;
    }
    if (a->has_min && b->has_max && a->min > b->max) {
      *out = 0;
      return true;
    }
    return false;
  case NYIR_CMP_GT:
    if (a->has_min && b->has_max && a->min > b->max) {
      *out = 1;
      return true;
    }
    if (a->has_max && b->has_min && a->max <= b->min) {
      *out = 0;
      return true;
    }
    return false;
  case NYIR_CMP_GE:
    if (a->has_min && b->has_max && a->min >= b->max) {
      *out = 1;
      return true;
    }
    if (a->has_max && b->has_min && a->max < b->min) {
      *out = 0;
      return true;
    }
    return false;
  }
  return false;
}

bool nir_range_excludes_zero(const nyr_range_t *r) {
  if (!r)
    return false;
  if (r->has_min && r->min > 0)
    return true;
  if (r->has_max && r->max < 0)
    return true;
  return false;
}

static bool nir_local_address_taken(const nyr_func_t *f, int64_t local) {
  if (!f || local < 0)
    return true;
  for (size_t i = 0; i < f->len; ++i) {
    const nyr_inst_t *in = &f->data[i];
    if (in->op == NYIR_ADDR_LOCAL && in->imm == local)
      return true;
  }
  return false;
}

static bool nir_inst_may_clobber_escaped_local(const nyr_inst_t *in) {
  unsigned effects = nyr_effective_effects(in);
  return (effects &
          (NYIR_EFFECT_WRITE_MEMORY | NYIR_EFFECT_UNKNOWN_SIDE_EFFECT |
           NYIR_EFFECT_FFI | NYIR_EFFECT_THREAD | NYIR_EFFECT_VOLATILE)) != 0;
}

bool nir_recover_load_local_range(const nyr_func_t *f,
                                  const nyr_value_fact_t *facts, int value,
                                  size_t at, nyr_range_t *out) {
  if (!f || !facts || !out || value < 0 || value >= f->next_value)
    return false;
  for (size_t j = at; j > 0; --j) {
    const nyr_inst_t *def = &f->data[j - 1];
    if (def->dst != value)
      continue;
    if (def->op != NYIR_LOAD_LOCAL)
      return false;
    bool escaped = nir_local_address_taken(f, def->imm);
    for (size_t k = j - 1; k > 0; --k) {
      const nyr_inst_t *store = &f->data[k - 1];
      if (store->op == NYIR_STORE_LOCAL && store->imm == def->imm) {
        if (store->a >= 0 && store->a < f->next_value &&
            (facts[store->a].range.has_min || facts[store->a].range.has_max)) {
          *out = facts[store->a].range;
          return true;
        }
        return false;
      }
      if (store->op == NYIR_LABEL || store->op == NYIR_BR ||
          store->op == NYIR_BR_IF)
        return false;
      if (escaped && nir_inst_may_clobber_escaped_local(store))
        return false;
    }
    return false;
  }
  return false;
}

static nyr_cmp_t nir_cmp_invert(nyr_cmp_t cmp) {
  switch (cmp) {
  case NYIR_CMP_EQ:
    return NYIR_CMP_NE;
  case NYIR_CMP_NE:
    return NYIR_CMP_EQ;
  case NYIR_CMP_LT:
    return NYIR_CMP_GE;
  case NYIR_CMP_LE:
    return NYIR_CMP_GT;
  case NYIR_CMP_GT:
    return NYIR_CMP_LE;
  case NYIR_CMP_GE:
    return NYIR_CMP_LT;
  }
  return cmp;
}

nyr_cmp_t nyr_cmp_swap_operands(nyr_cmp_t cmp) {
  switch (cmp) {
  case NYIR_CMP_LT:
    return NYIR_CMP_GT;
  case NYIR_CMP_LE:
    return NYIR_CMP_GE;
  case NYIR_CMP_GT:
    return NYIR_CMP_LT;
  case NYIR_CMP_GE:
    return NYIR_CMP_LE;
  default:
    return cmp;
  }
}

static int nir_strip_copy_value(const nyr_func_t *f, const int *defs,
                                int value) {
  for (unsigned depth = 0;
       f && defs && depth < 16 && value >= 0 && value < f->next_value;
       ++depth) {
    int di = defs[value];
    if (di < 0 || (size_t)di >= f->len || f->data[di].op != NYIR_COPY)
      break;
    value = f->data[di].a;
  }
  return value;
}

bool nyr_value_fact_singleton(const nyr_value_fact_t *fact, int64_t *value) {
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

bool nyr_inst_payload_free(const nyr_inst_t *in) {
  return in && !in->symbol && !in->debug.file && !in->extra_args &&
         !in->arg_sizes && !in->phi_incoming;
}

bool nyr_const_i64(const nyr_func_t *f, const int *defs, int value,
                    int64_t *out) {
  if (!f || !defs || value < 0 || value >= f->next_value || defs[value] < 0)
    return false;
  const nyr_inst_t *in = &f->data[(size_t)defs[value]];
  if (in->op != NYIR_CONST_I64)
    return false;
  if (out)
    *out = in->imm;
  return true;
}

static bool nir_fact_singleton(const nyr_value_fact_t *facts, size_t count,
                               int value, int64_t *out) {
  if (!facts || !out || value < 0 || (size_t)value >= count)
    return false;
  return nyr_value_fact_singleton(&facts[value], out);
}

static bool nir_intersect_cmp_bound(nyr_range_t *r, nyr_cmp_t cmp,
                                    int64_t c) {
  if (!r)
    return false;
  nyr_range_t next = *r;
  switch (cmp) {
  case NYIR_CMP_EQ:
    if ((next.has_min && c < next.min) || (next.has_max && c > next.max))
      return false;
    next = (nyr_range_t){.has_min = true, .has_max = true, .min = c, .max = c};
    break;
  case NYIR_CMP_NE:
    return false; /* a hole is not representable by one interval */
  case NYIR_CMP_LT:
    if (c == INT64_MIN)
      return false;
    if (!next.has_max || c - 1 < next.max) {
      next.has_max = true;
      next.max = c - 1;
    }
    break;
  case NYIR_CMP_LE:
    if (!next.has_max || c < next.max) {
      next.has_max = true;
      next.max = c;
    }
    break;
  case NYIR_CMP_GT:
    if (c == INT64_MAX)
      return false;
    if (!next.has_min || c + 1 > next.min) {
      next.has_min = true;
      next.min = c + 1;
    }
    break;
  case NYIR_CMP_GE:
    if (!next.has_min || c > next.min) {
      next.has_min = true;
      next.min = c;
    }
    break;
  }
  if (next.has_min && next.has_max && next.min > next.max)
    return false;
  *r = next;
  return true;
}

/*
 * Refine an interval along a chain of unique-predecessor CFG edges.  This is
 * intentionally local: joins retain the globally merged interval, while a
 * dominated true/false edge can recover the comparison bound that is valid
 * only on that path.  It lets BCE/peepholes consume branch facts without
 * making branch-local assumptions global metadata.
 */
static void nir_refine_range_from_edges(const nyr_func_t *f,
                                        const nyr_value_fact_t *facts,
                                        int value, size_t at,
                                        nyr_range_t *range) {
  if (!f || !facts || !range || at >= f->len || value < 0 ||
      value >= f->next_value)
    return;
  nyr_cfg_t cfg = {0};
  if (!nyr_cfg_build_topology(f, &cfg) || !cfg.block_count) {
    nyr_cfg_free(&cfg);
    return;
  }
  int *defs = nyr_build_defs(f);
  if (!defs) {
    nyr_cfg_free(&cfg);
    return;
  }
  int root_value = nir_strip_copy_value(f, defs, value);
  size_t child = cfg.inst_block[at];
  for (unsigned depth = 0; depth < 8 && child < cfg.block_count; ++depth) {
    size_t pb = cfg.pred_offsets[child], pe = cfg.pred_offsets[child + 1];
    if (pe - pb != 1)
      break;
    size_t pred = cfg.pred_blocks[pb];
    if (pred >= cfg.block_count || pred == child)
      break;
    size_t start = cfg.block_start[pred];
    size_t end = cfg.block_end[pred];
    if (end <= start) {
      child = pred;
      continue;
    }
    /*
     * Canonical NYIR commonly encodes an if/loop guard as BR_IF followed by
     * an explicit fallback BR.  Looking only at block_end-1 sees the fallback
     * and loses the path predicate.  Find the last conditional terminator and
     * separately resolve its false destination from the trailing BR or
     * fallthrough edge.
     */
    const nyr_inst_t *cond = NULL;
    const nyr_inst_t *fallback = NULL;
    for (size_t k = end; k > start; --k) {
      const nyr_inst_t *term = &f->data[k - 1];
      if (term->op == NYIR_NOP)
        continue;
      if (!fallback && term->op == NYIR_BR) {
        fallback = term;
        continue;
      }
      if (term->op == NYIR_BR_IF) {
        cond = term;
        break;
      }
      if (term->op != NYIR_LABEL)
        break;
    }
    if (cond && cond->a >= 0 && cond->a < f->next_value && defs[cond->a] >= 0) {
      int cdef = defs[cond->a];
      const nyr_inst_t *cmp = &f->data[(size_t)cdef];
      if (cmp->op == NYIR_CMP_I64) {
        bool true_edge = cfg.block_label[child] == cond->imm;
        bool false_edge = false;
        if (!true_edge) {
          if (fallback)
            false_edge = cfg.block_label[child] == fallback->imm;
          else
            false_edge = pred + 1 == child;
        }
        /*
         * When BOTH arms of the guard land in the same block (the CFG
         * builder dedups the identical edge, leaving a single
         * predecessor), the predicate is valid on only one path and is
         * not a block-wide fact -- refining here would delete guards
         * that fail on the other path.
         */
        if (!(true_edge && false_edge) && (true_edge || false_edge)) {
          int lhs = nir_strip_copy_value(f, defs, cmp->a);
          int rhs = nir_strip_copy_value(f, defs, cmp->b);
          nyr_cmp_t relation = cmp->cmp;
          int other = -1;
          if (lhs == root_value) {
            other = rhs;
          } else if (rhs == root_value) {
            other = lhs;
            relation = nyr_cmp_swap_operands(relation);
          }
          if (!true_edge)
            relation = nir_cmp_invert(relation);
          int64_t constant = 0;
          if (other >= 0 && nir_fact_singleton(facts, (size_t)f->next_value,
                                               other, &constant))
            (void)nir_intersect_cmp_bound(range, relation, constant);
        }
      }
    }
    child = pred;
  }
  free(defs);
  nyr_cfg_free(&cfg);
}

bool nir_value_range_at(const nyr_func_t *f, const nyr_value_fact_t *facts,
                        int value, size_t at, nyr_range_t *out) {
  if (!f || !facts || !out || value < 0 || value >= f->next_value)
    return false;
  bool have = false;
  if (facts[value].range.has_min || facts[value].range.has_max) {
    *out = facts[value].range;
    have = true;
  } else {
    have = nir_recover_load_local_range(f, facts, value, at, out);
  }
  if (!have)
    *out = (nyr_range_t){0};
  if (at < f->len)
    nir_refine_range_from_edges(f, facts, value, at, out);
  return out->has_min || out->has_max;
}

bool nir_operands_same_value(const nyr_func_t *f, int a, int b, size_t at) {
  if (a >= 0 && a == b)
    return true;
  if (!f || a < 0 || b < 0)
    return false;
  size_t ia = 0;
  size_t ib = 0;
  bool have_a = false;
  bool have_b = false;
  for (size_t j = at; j > 0 && (!have_a || !have_b); --j) {
    const nyr_inst_t *def = &f->data[j - 1];
    if (!have_a && def->dst == a) {
      ia = j - 1;
      have_a = true;
    }
    if (!have_b && def->dst == b) {
      ib = j - 1;
      have_b = true;
    }
  }
  if (!have_a || !have_b)
    return false;
  const nyr_inst_t *da = &f->data[ia];
  const nyr_inst_t *db = &f->data[ib];
  if (da->op == NYIR_COPY && da->a == b)
    return true;
  if (db->op == NYIR_COPY && db->a == a)
    return true;
  if (da->op == NYIR_COPY && db->op == NYIR_COPY && da->a >= 0 &&
      da->a == db->a)
    return true;
  if (da->op == NYIR_LOAD_LOCAL && db->op == NYIR_LOAD_LOCAL &&
      da->imm == db->imm) {
    size_t lo = ia < ib ? ia : ib;
    size_t hi = ia < ib ? ib : ia;
    int64_t local = da->imm;
    bool escaped = nir_local_address_taken(f, local);
    for (size_t k = lo + 1; k < hi; ++k) {
      const nyr_inst_t *mid = &f->data[k];
      if (mid->op == NYIR_STORE_LOCAL && mid->imm == local)
        return false;
      if (mid->op == NYIR_LABEL || mid->op == NYIR_BR || mid->op == NYIR_BR_IF)
        return false;
      if (escaped && nir_inst_may_clobber_escaped_local(mid))
        return false;
    }
    return true;
  }
  return false;
}

bool nir_range_excludes_int64_min(const nyr_range_t *r) {
  return r && r->has_min && r->min > INT64_MIN;
}

int nir_find_block_const0(const nyr_func_t *f, size_t at) {
  if (!f)
    return -1;
  int found = -1;
  for (size_t j = 0; j < at; ++j) {
    const nyr_inst_t *in = &f->data[j];
    if (in->op == NYIR_LABEL) {
      found = -1;
      continue;
    }
    if (in->op == NYIR_CONST_I64 && in->dst >= 0 && in->imm == 0)
      found = in->dst;
  }
  return found;
}

bool nir_rewrite_shl(nyr_func_t *f, size_t *i, int src, int shift) {
  if (!f || !i || *i >= f->len || src < 0 || shift <= 0 || shift >= 63)
    return false;
  int dst = f->data[*i].dst;
  if (dst < 0)
    return false;
  int shift_val = f->next_value++;
  if (f->len + 1 > f->cap) {
    size_t new_cap = f->cap ? f->cap * 2 : 64;
    if (new_cap < f->cap || new_cap > SIZE_MAX / sizeof(nyr_inst_t))
      return false;
    nyr_inst_t *new_data =
        (nyr_inst_t *)realloc(f->data, new_cap * sizeof(nyr_inst_t));
    if (!new_data)
      return false;
    f->data = new_data;
    f->cap = new_cap;
  }
  memmove(&f->data[*i + 1], &f->data[*i], (f->len - *i) * sizeof(nyr_inst_t));
  f->len++;
  f->data[*i] = (nyr_inst_t){.op = NYIR_CONST_I64,
                              .dst = shift_val,
                              .a = -1,
                              .b = -1,
                              .imm = (int64_t)shift};
  (*i)++;
  f->data[*i] = (nyr_inst_t){.op = NYIR_SHL_I64,
                              .dst = dst,
                              .a = src,
                              .b = shift_val,
                              .effects = NYIR_EFFECT_NONE};
  return true;
}

bool nir_rewrite_neg(nyr_func_t *f, size_t *i, int src) {
  if (!f || !i || *i >= f->len || src < 0)
    return false;
  int dst = f->data[*i].dst;
  if (dst < 0)
    return false;
  int zero = nir_find_block_const0(f, *i);
  if (zero < 0) {
    zero = f->next_value++;
    if (f->len + 1 > f->cap) {
      size_t new_cap = f->cap ? f->cap * 2 : 64;
      if (new_cap < f->cap || new_cap > SIZE_MAX / sizeof(nyr_inst_t))
        return false;
      nyr_inst_t *new_data =
          (nyr_inst_t *)realloc(f->data, new_cap * sizeof(nyr_inst_t));
      if (!new_data)
        return false;
      f->data = new_data;
      f->cap = new_cap;
    }
    memmove(&f->data[*i + 1], &f->data[*i],
            (f->len - *i) * sizeof(nyr_inst_t));
    f->len++;
    f->data[*i] = (nyr_inst_t){
        .op = NYIR_CONST_I64, .dst = zero, .a = -1, .b = -1, .imm = 0};
    (*i)++;
  }
  f->data[*i] = (nyr_inst_t){.op = NYIR_SUB_I64,
                              .dst = dst,
                              .a = zero,
                              .b = src,
                              .effects = NYIR_EFFECT_NONE};
  return true;
}

size_t nir_next_non_nop(const nyr_func_t *f, size_t start) {
  if (!f)
    return 0;
  for (size_t i = start; i < f->len; ++i) {
    if (f->data[i].op != NYIR_NOP)
      return i;
  }
  return f->len;
}

bool nir_remap_value(const int *map, int map_len, int value, int *out) {
  if (!out)
    return false;
  if (value < 0) {
    *out = value;
    return true;
  }
  if (!map || value >= map_len || map[value] < 0)
    return false;
  *out = map[value];
  return true;
}

bool nir_ensure_inst_space(nyr_func_t *f, size_t extra) {
  if (!f)
    return false;
  if (f->len + extra <= f->cap)
    return true;
  size_t new_cap = f->cap ? f->cap : 64;
  while (new_cap < f->len + extra)
    new_cap *= 2;
  nyr_inst_t *new_data =
      (nyr_inst_t *)realloc(f->data, new_cap * sizeof(nyr_inst_t));
  if (!new_data)
    return false;
  f->data = new_data;
  f->cap = new_cap;
  return true;
}

bool nyr_has_control_flow(const nyr_func_t *f) {
  if (!f)
    return false;
  for (size_t i = 0; i < f->len; ++i) {
    nyr_op_t op = f->data[i].op;
    if (op == NYIR_LABEL || op == NYIR_BR || op == NYIR_BR_IF)
      return true;
  }
  return false;
}

bool nyr_has_loop(const nyr_func_t *f) {
  if (!f || f->len == 0)
    return false;
  nyr_cfg_t cfg = {0};
  if (!nyr_cfg_build(f, &cfg))
    return false;
  bool has = false;
  if (cfg.backedge_edges) {
    for (size_t p = 0; p < cfg.block_count && !has; ++p) {
      for (size_t e = cfg.succ_offsets[p]; e < cfg.succ_offsets[p + 1]; ++e) {
        if (cfg.backedge_edges[e]) {
          has = true;
          break;
        }
      }
    }
  }
  nyr_cfg_free(&cfg);
  return has;
}

bool nyr_has_local_mem(const nyr_func_t *f) {
  if (!f)
    return false;
  for (size_t i = 0; i < f->len; ++i) {
    nyr_op_t op = f->data[i].op;
    if (op == NYIR_LOAD_LOCAL || op == NYIR_STORE_LOCAL ||
        op == NYIR_ADDR_LOCAL)
      return true;
  }
  return false;
}

/*
 * SSA def index: defs[v] = instruction index of the definition of value v,
 * or -1 when v has no in-function definition (parameter, undef, unseen call
 * result).  Returns NULL on allocation failure; callers free() the result.
 */
int *nyr_build_defs(const nyr_func_t *f) {
  if (!f || f->next_value <= 0)
    return NULL;
  int *defs = malloc((size_t)f->next_value * sizeof(*defs));
  if (!defs)
    return NULL;
  for (int v = 0; v < f->next_value; ++v)
    defs[v] = -1;
  for (size_t i = 0; i < f->len; ++i)
    if (f->data[i].dst >= 0 && f->data[i].dst < f->next_value)
      defs[f->data[i].dst] = (int)i;
  return defs;
}

size_t nyr_count_nops(const nyr_func_t *f) {
  size_t n = 0;
  if (!f)
    return 0;
  for (size_t i = 0; i < f->len; ++i)
    if (f->data[i].op == NYIR_NOP)
      n++;
  return n;
}

/*
 * Compaction threshold: compact when at least this fraction of the
 * instruction array is dead NOPs.  One eighth of the array.
 */
#define NYIR_COMPACT_SPARSE_DIVISOR 8u

bool nyr_compact_if_sparse(nyr_func_t *f) {
  if (!f || f->len == 0)
    return true;
  size_t nops = nyr_count_nops(f);
  /*
   * Compact when at least 1/NYIR_COMPACT_SPARSE_DIVISOR of the array is dead
   * NOPs (plus a floor of 2).  The former second arm (nops * 2 >= f->len)
   * was redundant under the OR: whenever the first arm fails, the second
   * fails too, so it could never be the deciding term and was removed.
   * ceil(f->len / divisor) is computed without a multiply so the threshold
   * can never overflow size_t.
   */
  size_t min_nops = f->len / NYIR_COMPACT_SPARSE_DIVISOR +
                    (f->len % NYIR_COMPACT_SPARSE_DIVISOR != 0);
  if (nops >= 2 && nops >= min_nops)
    return nyr_compact(f);
  return true;
}

size_t nyr_block_last_non_nop(const nyr_func_t *f, const nyr_cfg_t *cfg,
                               size_t block) {
  if (!f || !cfg || block >= cfg->block_count)
    return 0;
  size_t start = cfg->block_start[block];
  size_t end = cfg->block_end[block];
  if (end > f->len)
    end = f->len;
  while (end > start && f->data[end - 1].op == NYIR_NOP)
    --end;
  return end;
}

int64_t nyr_fresh_label(const nyr_func_t *f) {
  if (!f)
    return -1;
  int64_t max_label = -1;
  for (size_t i = 0; i < f->len; ++i) {
    const nyr_inst_t *in = &f->data[i];
    if ((in->op == NYIR_LABEL || in->op == NYIR_BR || in->op == NYIR_BR_IF) &&
        in->imm > max_label)
      max_label = in->imm;
    for (size_t k = 0; k < in->phi_incoming_len; ++k)
      if (in->phi_incoming[k].predecessor_label > max_label)
        max_label = in->phi_incoming[k].predecessor_label;
  }
  return max_label == INT64_MAX ? -1 : max_label + 1;
}

size_t nyr_block_count(const nyr_func_t *f) {
  if (!f || f->len == 0)
    return 0;
  size_t count = 1;
  for (size_t i = 1; i < f->len; ++i) {
    nyr_op_t prev = f->data[i - 1].op;
    if (f->data[i].op == NYIR_LABEL || prev == NYIR_BR || prev == NYIR_BR_IF ||
        prev == NYIR_RET)
      ++count;
  }
  return count;
}

uint64_t nyr_debug_fingerprint(const nyr_func_t *f) {
  uint64_t hash = UINT64_C(1469598103934665603);
  if (!f)
    return hash;
#define NIR_HASH(v)                                                            \
  do {                                                                         \
    hash ^= (uint64_t)(v);                                                     \
    hash *= UINT64_C(1099511628211);                                           \
  } while (0)
  NIR_HASH(f->len);
  NIR_HASH(f->next_value);
  NIR_HASH(f->param_count);
  for (size_t i = 0; i < f->param_count; ++i)
    NIR_HASH(f->param_types ? f->param_types[i] : NYIR_PARAM_I64);
  for (size_t i = 0; i < f->param_count; ++i)
    NIR_HASH(f->param_abi ? f->param_abi[i] : 0);
  NIR_HASH(f->return_aggregate_size);
  NIR_HASH(f->return_aggregate_classes[0]);
  NIR_HASH(f->return_aggregate_classes[1]);
  NIR_HASH(f->source_param_count);
  for (size_t i = 0; i < f->source_param_count; ++i)
    NIR_HASH(f->source_param_widths ? f->source_param_widths[i] : 0);
  for (size_t i = 0; i < f->len; ++i) {
    const nyr_inst_t *in = &f->data[i];
    NIR_HASH(in->op);
    NIR_HASH(in->cmp);
    NIR_HASH(in->dst);
    NIR_HASH(in->a);
    NIR_HASH(in->b);
    NIR_HASH(in->c);
    NIR_HASH(in->d);
    NIR_HASH(in->e);
    NIR_HASH(in->f);
    NIR_HASH(in->imm);
    NIR_HASH(in->flags);
    NIR_HASH(in->effects);
    NIR_HASH(in->scalar_class);
    NIR_HASH(in->semantic_ownership);
    NIR_HASH(in->semantic_mutable);
    NIR_HASH(in->alias_class);
    /*
     * Range facts are consumed by legality checks in SCCP/VRP, bounds
     * elimination, strength reduction, and loop transforms. A pass that only
     * changes a fact must therefore count as a semantic IR change.
     */
    NIR_HASH(in->range.has_min);
    NIR_HASH(in->range.has_max);
    NIR_HASH(in->range.min);
    NIR_HASH(in->range.max);
    /*
     * Source locations are part of the diagnostic IR state. A pass that
     * moves or drops a location can change which source line an optimizer or
     * backend failure points at, even when executable instructions match.
     */
    NIR_HASH(in->debug.file != NULL);
    if (in->debug.file) {
      for (const unsigned char *p = (const unsigned char *)in->debug.file; *p;
           ++p)
        NIR_HASH(*p);
    }
    NIR_HASH(0xfeu);
    NIR_HASH(in->debug.line);
    NIR_HASH(in->debug.column);
    NIR_HASH(in->ret_aggregate_size);
    NIR_HASH(in->ret_aggregate_classes[0]);
    NIR_HASH(in->ret_aggregate_classes[1]);
    for (size_t k = 0;
         k < sizeof(in->arg_scalar_classes) / sizeof(in->arg_scalar_classes[0]);
         ++k)
      NIR_HASH(in->arg_scalar_classes[k]);
    for (size_t k = 0; k < NYIR_CALL_MAX_ARGS / 32; ++k)
      NIR_HASH(in->arg_ownership[k]);
    for (size_t k = 0; k < NYIR_CALL_MAX_ARGS / 32; ++k)
      NIR_HASH(in->arg_contracts[k]);
    if (in->symbol) {
      for (const unsigned char *p = (const unsigned char *)in->symbol; *p; ++p)
        NIR_HASH(*p);
    }
    NIR_HASH(0xffu);
    NIR_HASH(in->extra_args_len);
    NIR_HASH(in->phi_incoming_len);
    for (size_t k = 0; k < in->extra_args_len; ++k)
      NIR_HASH(in->extra_args[k]);
    for (size_t k = 0; k < in->phi_incoming_len; ++k) {
      NIR_HASH(in->phi_incoming[k].predecessor_label);
      NIR_HASH(in->phi_incoming[k].value);
    }
    if (in->arg_sizes && in->op == NYIR_CALL && in->imm >= 0 &&
        in->imm <= NYIR_CALL_MAX_ARGS)
      for (size_t k = 0; k < (size_t)in->imm; ++k)
        NIR_HASH(in->arg_sizes[k]);
  }
#undef NIR_HASH
  return hash;
}

void nyr_pass_tag(char *buf, size_t n, const char *pass_name) {
  if (!buf || n < 4) {
    if (buf && n)
      buf[0] = '\0';
    return;
  }
  size_t j = 0;
  buf[j++] = '<';
  for (size_t i = 0; pass_name && pass_name[i] && j + 2 < n; ++i) {
    char c = pass_name[i];
    if (c == '_' || c == ' ')
      c = '-';
    else if (c == '(' || c == ')')
      continue;
    buf[j++] = c;
  }
  buf[j++] = '>';
  buf[j] = '\0';
}

/*
 * Fold a float binary operation when both operands are constant.  Returns true
 * and writes the bit-pattern result into *out_imm and *out_kind when the fold
 * is valid (no div-by-zero).
 */
bool nyr_float_fold_binary(nyr_op_t op, int64_t a_imm, int64_t b_imm,
                            unsigned char kind, int64_t *out_imm,
                            unsigned char *out_kind) {
  if (kind != 2 && kind != 3)
    return false;
  bool is_f32 = kind == 3;
  double av, bv, r = 0.0;
  if (is_f32) {
    uint32_t au = (uint32_t)(unsigned)a_imm;
    uint32_t bu = (uint32_t)(unsigned)b_imm;
    float af, bf;
    memcpy(&af, &au, sizeof(af));
    memcpy(&bf, &bu, sizeof(bf));
    av = (double)af;
    bv = (double)bf;
  } else {
    memcpy(&av, &a_imm, sizeof(av));
    memcpy(&bv, &b_imm, sizeof(bv));
  }
  bool ok = false;
  switch (op) {
  case NYIR_ADD_F64:
  case NYIR_ADD_F32:
    r = av + bv;
    ok = true;
    break;
  case NYIR_SUB_F64:
  case NYIR_SUB_F32:
    r = av - bv;
    ok = true;
    break;
  case NYIR_MUL_F64:
  case NYIR_MUL_F32:
    r = av * bv;
    ok = true;
    break;
  case NYIR_DIV_F64:
    if (bv != 0.0) {
      r = av / bv;
      ok = true;
    }
    break;
  case NYIR_DIV_F32:
    if ((float)bv != 0.0f) {
      r = (double)((float)av / (float)bv);
      ok = true;
    }
    break;
  default:
    break;
  }
  if (!ok)
    return false;
  if (is_f32) {
    float rf = (float)r;
    uint32_t bits = 0;
    memcpy(&bits, &rf, sizeof(bits));
    *out_imm = (int64_t)bits;
    *out_kind = 3;
  } else {
    memcpy(out_imm, &r, sizeof(*out_imm));
    *out_kind = 2;
  }
  return true;
}
