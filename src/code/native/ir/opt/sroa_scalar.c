/*
 * SROA Scalar Replacement: replaces non-escaping local slots with direct SSA values.
 *
 * This pass runs after escape_sroa and performs the actual replacement of
 * LOAD_LOCAL/STORE_LOCAL pairs with direct value propagation (COPY instructions
 * or direct operand use).
 */
#include "code/native/ir/opt/util.h"
#include <stdlib.h>
#include <string.h>

bool nyir_sroa_scalar(nyir_func_t *f);

bool nyir_sroa_scalar(nyir_func_t *f) {
  if (!f || f->len == 0)
    return true;

  size_t slot_count = nyir_local_slot_count(f);
  if (!slot_count)
    return true;
  nyir_local_escape_info_t stk_info[64] = {0};
  int stk_def[64];
  nyir_local_escape_info_t *info = slot_count <= 64 ? stk_info : calloc(slot_count, sizeof(*info));
  if (!info || !nyir_analyze_local_escapes(f, info, slot_count)) {
    if (slot_count > 64) free(info);
    return false;
  }

  /*
   * Basic-block-local value tracking; control-flow/calls end availability.
   */
  int *current_def = slot_count <= 64 ? stk_def : malloc(slot_count * sizeof(*current_def));
  if (!current_def) {
    if (slot_count > 64) free(info);
    return false;
  }
  for (size_t i = 0; i < slot_count; ++i)
    current_def[i] = -1;

  bool any_replaced = false;
  for (size_t i = 0; i < f->len; ++i) {
    nyir_inst_t *in = &f->data[i];
    if (in->op == NYIR_LABEL || in->op == NYIR_BR ||
        in->op == NYIR_BR_IF) {
      for (size_t k = 0; k < slot_count; ++k)
        current_def[k] = -1;
      continue;
    }
    if (in->op == NYIR_STORE_LOCAL && in->imm >= 0 &&
        (size_t)in->imm < slot_count && !info[in->imm].escapes) {
      current_def[in->imm] = in->a;
    } else if (in->op == NYIR_LOAD_LOCAL && in->imm >= 0 &&
               (size_t)in->imm < slot_count && !info[in->imm].escapes) {
      size_t slot = (size_t)in->imm;
      if (current_def[slot] >= 0 && in->dst >= 0) {
        nir_make_copy(in, current_def[slot]);
        any_replaced = true;
      }
    }
  }

  /*
   * Remove private stores that have no same-region load before overwrite.
   */
  if (any_replaced) {
    for (size_t i = 0; i < f->len; ++i) {
      nyir_inst_t *in = &f->data[i];
      if (in->op != NYIR_STORE_LOCAL || in->imm < 0 ||
          (size_t)in->imm >= slot_count || info[in->imm].escapes)
        continue;
      bool has_load = false;
      for (size_t j = i + 1; j < f->len; ++j) {
        nyir_inst_t *next = &f->data[j];
        if (next->op == NYIR_LABEL || next->op == NYIR_BR ||
            next->op == NYIR_BR_IF)
          break;
        if (next->op == NYIR_LOAD_LOCAL && next->imm == in->imm) {
          has_load = true;
          break;
        }
        if (next->op == NYIR_STORE_LOCAL && next->imm == in->imm)
          break;
      }
      if (!has_load) {
        *in = (nyir_inst_t){.op = NYIR_NOP, .dst = -1, .a = -1, .b = -1,
                            .c = -1, .d = -1, .e = -1, .f = -1};
      }
    }
    (void)nyir_compact_if_sparse(f);
  }

  if (slot_count > 64) {
    free(current_def);
    free(info);
  }
  return true;
}
