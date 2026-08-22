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
#include "code/native/ir/opt/util.h"
#include <stdlib.h>
#include <string.h>

typedef struct {
  size_t insert_at; /* f->data index to insert the store before */
  int slot;         /* local slot written */
  int value;        /* value stored (defined at the insert point) */
} nyir_phi_store_t;

bool nyir_phi_elim(nyir_func_t *f) {
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

  /*
   * Block map: next_label[i] is the index of the next NYIR_LABEL at or
   * after i (f->len when none).  The block starting at a label position
   * p occupies [p, next_label[p + 1]).  The entry block (which has no
   * label instruction) occupies [0, first_label).
   */
  size_t stk_next_lbl[257] = {0};
  size_t *next_label = f->len <= 256 ? stk_next_lbl : (size_t *)calloc(f->len + 1, sizeof(*next_label));
  if (!next_label)
    return true;
  next_label[f->len] = f->len;
  for (size_t i = f->len; i-- > 0;)
    next_label[i] = f->data[i].op == NYIR_LABEL ? i : next_label[i + 1];
  size_t first_label = next_label[0];

  nyir_phi_store_t *stores = NULL;
  size_t store_count = 0, store_cap = 0;
  /*
   * Fresh slots start past every existing local reference so they can
   * never alias a program local.
   */
  int slot = (int)nyir_max_local(f);
  bool ok = true;

  for (size_t i = 0; i < f->len; ++i) {
    nyir_inst_t *in = &f->data[i];
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
          nyir_op_t op = f->data[t].op;
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
          nyir_op_t op = f->data[t].op;
          if (op == NYIR_BR || op == NYIR_BR_IF || op == NYIR_RET) {
            insert_at = t;
            break;
          }
        }
      }

      if (store_count == store_cap) {
        size_t ncap = store_cap ? store_cap * 2 : 8;
        nyir_phi_store_t *ns =
            (nyir_phi_store_t *)realloc(stores, ncap * sizeof(*ns));
        if (!ns) {
          ok = false;
          goto done;
        }
        stores = ns;
        store_cap = ncap;
      }
      stores[store_count++] =
          (nyir_phi_store_t){.insert_at = insert_at, .slot = slot,
                             .value = value};
    }

    /*
     * Rewrite the PHI into a single header load (keeps SSA single-def).
     */
    int dst = in->dst;
    free(in->phi_incoming);
    *in = (nyir_inst_t){.op = NYIR_LOAD_LOCAL, .dst = dst, .imm = slot};
    ++slot;
  }

  /*
   * Insert stores newest-position-first so earlier insert points stay
   * valid (inserting at position p shifts only indices >= p).
   */
  for (size_t i = 1; i < store_count; ++i) {
    nyir_phi_store_t key = stores[i];
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
    memmove(&f->data[at + 1], &f->data[at],
            (f->len - at) * sizeof(nyir_inst_t));
    f->data[at] = (nyir_inst_t){.op = NYIR_STORE_LOCAL,
                                .a = stores[i].value, .imm = stores[i].slot};
    ++f->len;
  }

done:
  free(stores);
  if (f->len > 256) free(next_label);
  return ok;
}
