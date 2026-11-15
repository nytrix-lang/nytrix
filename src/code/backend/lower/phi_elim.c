/*
 * PHI elimination for direct-NYIR backends.
 *
 * The NYIR optimizer produces SSA PHIs at O2/O3, but the direct-NYIR
 * encoders (x86-64 assembler, object encoders, portable/stack backends)
 * have no NYIR_PHI support.  This pass runs as the last optimization
 * step and lowers every PHI through memory while keeping the
 * single-definition property the NYIR verifier enforces:
 *
 *   dst = phi (L_a: v_a, L_b: v_b)   ->
 *       store local S, v_a        (end of block L_a)
 *       store local S, v_b        (end of block L_b)
 *       load local S -> dst       (block header, replaces the PHI)
 *
 * Each PHI gets a fresh local slot, so every vreg still has exactly one
 * definition (the header load).  Incoming values are read at the
 * predecessor terminator, where they are guaranteed to be defined (the
 * verifier checks PHI edge operands), so there is no parallel-copy
 * hazard: stores only write memory, loads only happen at the header
 * after the branch, and cross-dependent PHIs (v1 = phi(.., v2),
 * v2 = phi(.., v1)) read the live SSA values of their own block.
 */
#include "code/backend/passes/util.h"
#include <stdlib.h>
#include <string.h>

typedef struct {
  size_t insert_at; /* f->data index to insert the store before */
  int slot;         /* local slot written */
  int vec_addr;     /* 32-byte home of a four-lane i64 PHI, or -1 */
  int value;        /* value stored (defined at the insert point) */
  nyr_scalar_class_t scalar_class;
  nyr_debug_loc_t debug;
} nyr_phi_store_t;

/*
 * Keep the machine class attached to the actual incoming definitions. The
 * type map is intentionally conservative/coarse and can mark a pointer phi as
 * f64 when values share a slot with a float along another path.
 */
static nyr_scalar_class_t nyr_phi_value_class(const nyr_func_t *f,
                                              const nyr_type_map_t *types,
                                              int value, unsigned depth) {
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
  if (def->scalar_class != NYIR_SCALAR_UNKNOWN)
    return def->scalar_class;
  if (def->op == NYIR_COPY)
    return nyr_phi_value_class(f, types, def->a, depth + 1);
  if (def->op == NYIR_PHI && def->phi_incoming_len) {
    nyr_scalar_class_t joined = NYIR_SCALAR_UNKNOWN;
    for (size_t i = 0; i < def->phi_incoming_len; ++i) {
      nyr_scalar_class_t current =
          nyr_phi_value_class(f, types, def->phi_incoming[i].value, depth + 1);
      if (current == NYIR_SCALAR_UNKNOWN ||
          (joined != NYIR_SCALAR_UNKNOWN && joined != current))
        return NYIR_SCALAR_UNKNOWN;
      joined = current;
    }
    return joined;
  }
  /*
   * LOAD_LOCAL and other representation-preserving producers can have no
   * explicit scalar class even when their unique inferred representation is
   * floating-point. Use only unambiguous type-map facts; mixed roots remain
   * unknown, preserving the conservative behavior for pointer/float aliases.
   */
  if (types && (size_t)value < types->value_count) {
    bool f32 = types->value_f32[value];
    bool f64 = types->value_f64[value];
    if (f32 != f64)
      return f64 ? NYIR_SCALAR_F64 : NYIR_SCALAR_F32;
  }
  return NYIR_SCALAR_UNKNOWN;
}

/*
 * Four-lane i64 values (VEC8_*_I64) are 32 bytes; an 8-byte local slot would
 * keep only lane 0. Such PHIs are lowered through a 32-byte stack home
 * instead (VEC8_STORE_I64 on each edge, VEC8_LOAD_I64 in the header).
 */
static bool nyr_phi_value_is_vec8_i64(const nyr_func_t *f, int value,
                                      const size_t *defs, size_t *seen,
                                      size_t generation, int *pending) {
  if (!f || value < 0 || value >= f->next_value)
    return false;
  size_t pending_count = 0;
  seen[value] = generation;
  pending[pending_count++] = value;
  while (pending_count) {
    int current = pending[--pending_count];
    size_t def_at = defs[current];
    if (def_at == SIZE_MAX)
      continue;
    const nyr_inst_t *def = &f->data[def_at];
    switch (def->op) {
    case NYIR_VEC8_LOAD_I64:
    case NYIR_VEC8_ADD_I64:
    case NYIR_VEC8_SUB_I64:
    case NYIR_VEC8_AND_I64:
    case NYIR_VEC8_OR_I64:
    case NYIR_VEC8_XOR_I64:
    case NYIR_VEC8_SET1_I64:
    case NYIR_VEC8_SAR_I64:
    case NYIR_VEC8_SHR_I64:
      return true;
    case NYIR_COPY:
      if (def->a >= 0 && def->a < f->next_value && seen[def->a] != generation) {
        seen[def->a] = generation;
        pending[pending_count++] = def->a;
      }
      break;
    case NYIR_PHI:
      for (size_t k = 0; k < def->phi_incoming_len; ++k) {
        int incoming = def->phi_incoming[k].value;
        if (incoming >= 0 && incoming < f->next_value &&
            seen[incoming] != generation) {
          seen[incoming] = generation;
          pending[pending_count++] = incoming;
        }
      }
      break;
    default:
      break;
    }
  }
  return false;
}

/*
 * Give every four-lane i64 PHI a 32-byte ALLOCA home at function entry.
 * `homes[dst]` receives the home address value; other entries stay -1.
 */
static int *nyr_phi_vec8_homes(nyr_func_t *f, bool *ok) {
  size_t value_count = (size_t)f->next_value;
  if (!value_count)
    return NULL;

  size_t count = 0;
  bool has_vec8 = false;
  for (size_t i = 0; i < f->len; ++i) {
    nyr_op_t op = f->data[i].op;
    has_vec8 |= op == NYIR_VEC8_LOAD_I64 || op == NYIR_VEC8_ADD_I64 ||
                op == NYIR_VEC8_SUB_I64 || op == NYIR_VEC8_AND_I64 ||
                op == NYIR_VEC8_OR_I64 || op == NYIR_VEC8_XOR_I64 ||
                op == NYIR_VEC8_SET1_I64 || op == NYIR_VEC8_SAR_I64 ||
                op == NYIR_VEC8_SHR_I64;
  }
  if (!has_vec8)
    return NULL;

  size_t *defs = calloc(value_count, sizeof(*defs));
  size_t *seen = calloc(value_count, sizeof(*seen));
  int *pending = calloc(value_count, sizeof(*pending));
  bool *vec8_values = calloc(value_count, sizeof(*vec8_values));
  if (!defs || !seen || !pending || !vec8_values) {
    free(defs);
    free(seen);
    free(pending);
    free(vec8_values);
    *ok = false;
    return NULL;
  }
  for (size_t i = 0; i < value_count; ++i)
    defs[i] = SIZE_MAX;
  for (size_t i = f->len; i > 0; --i) {
    int dst = f->data[i - 1].dst;
    if (dst >= 0 && (size_t)dst < value_count && defs[dst] == SIZE_MAX)
      defs[dst] = i - 1;
  }
  size_t generation = 0;
  for (size_t i = 0; i < f->len; ++i) {
    if (f->data[i].op != NYIR_PHI)
      continue;
    if (++generation == 0) {
      memset(seen, 0, value_count * sizeof(*seen));
      generation = 1;
    }
    int dst = f->data[i].dst;
    if (nyr_phi_value_is_vec8_i64(f, dst, defs, seen, generation, pending)) {
      vec8_values[dst] = true;
      ++count;
    }
  }
  if (!count) {
    free(defs);
    free(seen);
    free(pending);
    free(vec8_values);
    return NULL;
  }

  int *phi_dst = calloc(count, sizeof(*phi_dst));
  if (!phi_dst) {
    free(defs);
    free(seen);
    free(pending);
    free(vec8_values);
    *ok = false;
    return NULL;
  }
  size_t phi_count = 0;
  for (size_t i = 0; i < f->len; ++i) {
    int dst = f->data[i].dst;
    if (f->data[i].op == NYIR_PHI && dst >= 0 && (size_t)dst < value_count &&
        vec8_values[dst])
      phi_dst[phi_count++] = dst;
  }
  free(vec8_values);

  int value_limit = f->next_value;
  int *homes = calloc(value_count, sizeof(*homes));
  if (!homes) {
    free(defs);
    free(seen);
    free(pending);
    free(phi_dst);
    *ok = false;
    return NULL;
  }
  for (int v = 0; v < value_limit; ++v)
    homes[v] = -1;
  for (size_t k = 0; k < phi_count; ++k) {
    int dst = phi_dst[k];
    nyr_inst_t home = {.op = NYIR_ALLOCA,
                       .dst = -1,
                       .a = -1,
                       .b = -1,
                       .c = -1,
                       .d = -1,
                       .e = -1,
                       .f = -1,
                       .imm = 32};
    size_t before = f->len;
    int addr = nyr_ir_emit(f, home);
    if (addr < 0 || f->len != before + 1) {
      free(homes);
      free(defs);
      free(seen);
      free(pending);
      free(phi_dst);
      *ok = false;
      return NULL;
    }
    /*
     * Move the home allocation to the entry: it must run exactly once.
     */
    nyr_inst_t moved = f->data[f->len - 1];
    memmove(&f->data[1], &f->data[0], (f->len - 1) * sizeof(*f->data));
    f->data[0] = moved;
    homes[dst] = addr;
  }
  free(phi_dst);
  free(defs);
  free(seen);
  free(pending);
  return homes;
}

bool nyr_phi_elim(nyr_func_t *f) {
  if (!f || f->len == 0)
    return true;

  bool has_phi = false;
  for (size_t i = 0; i < f->len; ++i) {
    if (f->data[i].op == NYIR_PHI) {
      has_phi = true;
      break;
    }
  }
  if (!has_phi)
    return true;
  bool homes_ok = true;
  int *vec_homes = nyr_phi_vec8_homes(f, &homes_ok);
  if (!homes_ok)
    return false;
  const int vec_home_limit = f->next_value;

  /*
   * Block map: next_label[i] is the index of the next NYIR_LABEL at or
   * after i (f->len when none).  The block starting at a label position
   * p occupies [p, next_label[p + 1]).  The entry block (which has no
   * label instruction) occupies [0, first_label).
   */
  size_t stk_next_lbl[257] = {0};
  size_t *next_label = f->len <= 256
                           ? stk_next_lbl
                           : (size_t *)calloc(f->len + 1, sizeof(*next_label));
  if (!next_label) {
    free(vec_homes);
    return true;
  }
  next_label[f->len] = f->len;
  for (size_t i = f->len; i-- > 0;)
    next_label[i] = f->data[i].op == NYIR_LABEL ? i : next_label[i + 1];
  size_t first_label = next_label[0];

  nyr_phi_store_t *stores = NULL;
  size_t store_count = 0, store_cap = 0;
  /*
   * Fresh slots start past every existing local reference so they can
   * never alias a program local.
   */
  int slot = (int)nyr_max_local(f);
  bool ok = true;

  /*
   * Unlabeled (-1) PHI predecessors may name the entry block, but
   * mid-function fallthrough blocks after BR_IF are unlabeled too.
   * Routing every -1 edge's store into the entry block misplaces it
   * (wrong value or use-before-def). Accept the entry-fallthrough shape
   * only when there is exactly one -1 incoming overall and its value is
   * defined inside the entry region; otherwise leave the PHIs untouched
   * (the native machine path lowers them itself).
   */
  {
    size_t neg_count = 0;
    int neg_value = -1;
    for (size_t i = 0; i < f->len; ++i) {
      const nyr_inst_t *in = &f->data[i];
      if (in->op != NYIR_PHI || !in->phi_incoming)
        continue;
      for (size_t k = 0; k < in->phi_incoming_len; ++k) {
        if (in->phi_incoming[k].predecessor_label < 0) {
          ++neg_count;
          neg_value = in->phi_incoming[k].value;
        }
      }
    }
    if (neg_count > 1) {
      free(vec_homes);
      if (next_label != stk_next_lbl)
        free(next_label);
      return true;
    }
    if (neg_count == 1 && neg_value >= 0) {
      bool defined_in_entry = false;
      for (size_t j = 0; j < first_label; ++j)
        if (f->data[j].dst == neg_value) {
          defined_in_entry = true;
          break;
        }
      if (!defined_in_entry) {
        free(vec_homes);
        if (next_label != stk_next_lbl)
          free(next_label);
        return true;
      }
    }
  }

  nyr_type_map_t value_types = {0};
  if (!nyr_type_map_init(&value_types, f, nyr_max_local(f))) {
    free(vec_homes);
    if (next_label != stk_next_lbl)
      free(next_label);
    return true;
  }

  for (size_t i = 0; i < f->len; ++i) {
    nyr_inst_t *in = &f->data[i];
    if (in->op != NYIR_PHI)
      continue;

    /*
     * Header position of this PHI: the nearest preceding label.
     */
    size_t header_pos = SIZE_MAX;
    for (size_t j = i; j-- > 0;) {
      if (f->data[j].op == NYIR_LABEL) {
        header_pos = j;
        break;
      }
    }

    for (size_t k = 0; k < in->phi_incoming_len; ++k) {
      int64_t pred = in->phi_incoming[k].predecessor_label;
      int value = in->phi_incoming[k].value;
      size_t insert_at = SIZE_MAX;

      /*
       * A label can cover several CFG blocks: the fallthrough after BR_IF
       * need not have its own NYIR_LABEL.  The PHI predecessor names the
       * block that starts at the label, so its edge terminator is the first
       * terminator after that label, not the last one before the next label.
       * Choosing the last terminator skips the store when an earlier BR_IF
       * jumps directly to the PHI header.
       */
      for (size_t j = 0; j < f->len; ++j) {
        if (f->data[j].op != NYIR_LABEL || f->data[j].imm != pred)
          continue;
        size_t pend = next_label[j + 1];
        insert_at = pend;
        for (size_t t = j + 1; t < pend; ++t) {
          nyr_op_t op = f->data[t].op;
          if (op == NYIR_BR || op == NYIR_BR_IF || op == NYIR_RET) {
            insert_at = t;
            break;
          }
        }
        break;
      }
      if (insert_at == SIZE_MAX) {
        /*
         * Unlabeled predecessor: the entry block.  The edge into the
         * header is either the entry block's fallthrough (the header is
         * the first labeled block) or its terminator branch.  Bound the
         * scan to the entry block itself, [0, first_label): a header
         * beyond another labeled block must be reached by an explicit
         * branch, which lives inside the entry block.
         */
        if (header_pos == SIZE_MAX)
          continue; /* defensively skip a malformed shape */
        insert_at = first_label;
        for (size_t t = 0; t < first_label; ++t) {
          nyr_op_t op = f->data[t].op;
          if (op == NYIR_BR || op == NYIR_BR_IF || op == NYIR_RET) {
            insert_at = t;
            break;
          }
        }
      }

      if (store_count == store_cap) {
        size_t ncap = store_cap ? store_cap * 2 : 8;
        nyr_phi_store_t *ns =
            (nyr_phi_store_t *)realloc(stores, ncap * sizeof(*ns));
        if (!ns) {
          ok = false;
          goto done;
        }
        stores = ns;
        store_cap = ncap;
      }
      nyr_scalar_class_t scalar_class =
          nyr_phi_value_class(f, &value_types, value, 0);
      int vec_addr = vec_homes && in->dst >= 0 && in->dst < vec_home_limit
                         ? vec_homes[in->dst]
                         : -1;
      stores[store_count++] = (nyr_phi_store_t){.insert_at = insert_at,
                                                .slot = slot,
                                                .vec_addr = vec_addr,
                                                .value = value,
                                                .scalar_class = scalar_class,
                                                .debug = in->debug};
    }

    /*
     * Rewrite the PHI into a single header load (keeps SSA single-def).
     */
    int dst = in->dst;
    nyr_scalar_class_t result_class = in->scalar_class;
    uint8_t semantic_ownership = in->semantic_ownership;
    bool semantic_mutable = in->semantic_mutable;
    nyr_debug_loc_t debug = in->debug;
    nyr_range_t range = in->range;
    if (result_class == NYIR_SCALAR_UNKNOWN && in->phi_incoming_len) {
      for (size_t k = 0; k < in->phi_incoming_len; ++k) {
        nyr_scalar_class_t incoming =
            nyr_phi_value_class(f, &value_types, in->phi_incoming[k].value, 0);
        if (incoming == NYIR_SCALAR_UNKNOWN ||
            (result_class != NYIR_SCALAR_UNKNOWN && result_class != incoming)) {
          result_class = NYIR_SCALAR_UNKNOWN;
          break;
        }
        result_class = incoming;
      }
    }
    free(in->phi_incoming);
    int vec_addr =
        vec_homes && dst >= 0 && dst < vec_home_limit ? vec_homes[dst] : -1;
    if (vec_addr >= 0) {
      *in = (nyr_inst_t){.op = NYIR_VEC8_LOAD_I64,
                         .dst = dst,
                         .a = vec_addr,
                         .b = -1,
                         .c = -1,
                         .d = -1,
                         .e = -1,
                         .f = -1,
                         .effects = NYIR_EFFECT_READ_MEMORY |
                                    NYIR_EFFECT_MAY_TRAP,
                         .debug = debug};
      continue;
    }
    *in = (nyr_inst_t){.op = NYIR_LOAD_LOCAL,
                       .dst = dst,
                       .imm = slot,
                       .scalar_class = result_class,
                       .semantic_ownership = semantic_ownership,
                       .semantic_mutable = semantic_mutable,
                       .alias_class = 0,
                       .effects = NYIR_EFFECT_READ_LOCAL,
                       .debug = debug,
                       .range = range};
    ++slot;
  }

  /*
   * Insert stores newest-position-first so earlier insert points stay
   * valid (inserting at position p shifts only indices >= p).
   */
  for (size_t i = 1; i < store_count; ++i) {
    nyr_phi_store_t key = stores[i];
    size_t j = i;
    while (j > 0 && stores[j - 1].insert_at < key.insert_at) {
      stores[j] = stores[j - 1];
      --j;
    }
    stores[j] = key;
  }
  for (size_t i = 0; i < store_count; ++i) {
    size_t at = stores[i].insert_at;
    if (!nir_ensure_inst_space(f, 1)) {
      ok = false;
      goto done;
    }
    memmove(&f->data[at + 1], &f->data[at], (f->len - at) * sizeof(nyr_inst_t));
    if (stores[i].vec_addr >= 0) {
      f->data[at] = (nyr_inst_t){.op = NYIR_VEC8_STORE_I64,
                                 .dst = -1,
                                 .a = stores[i].vec_addr,
                                 .b = stores[i].value,
                                 .c = -1,
                                 .d = -1,
                                 .e = -1,
                                 .f = -1,
                                 .effects = NYIR_EFFECT_WRITE_MEMORY |
                                            NYIR_EFFECT_MAY_TRAP,
                                 .debug = stores[i].debug};
      ++f->len;
      continue;
    }
    f->data[at] = (nyr_inst_t){.op = NYIR_STORE_LOCAL,
                               .dst = -1,
                               .a = stores[i].value,
                               .b = -1,
                               .c = -1,
                               .d = -1,
                               .e = -1,
                               .f = -1,
                               .imm = stores[i].slot,
                               .scalar_class = stores[i].scalar_class,
                               .effects = NYIR_EFFECT_WRITE_LOCAL,
                               .debug = stores[i].debug};
    ++f->len;
  }

done:
  nyr_type_map_free(&value_types);
  free(vec_homes);
  free(stores);
  if (next_label != stk_next_lbl)
    free(next_label);
  return ok;
}
